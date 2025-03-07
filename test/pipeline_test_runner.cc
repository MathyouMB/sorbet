#include "doctest.h"
#include <cxxopts.hpp>
// has to go first as it violates our requirements

// has to go first, as it violates poisons
#include "core/proto/proto.h"

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "ast/ast.h"
#include "ast/desugar/Desugar.h"
#include "ast/treemap/treemap.h"
#include "cfg/CFG.h"
#include "cfg/builder/builder.h"
#include "class_flatten/class_flatten.h"
#include "common/FileOps.h"
#include "common/common.h"
#include "common/formatting.h"
#include "common/sort.h"
#include "common/web_tracer_framework/tracing.h"
#include "core/Error.h"
#include "core/ErrorCollector.h"
#include "core/ErrorQueue.h"
#include "core/Unfreeze.h"
#include "core/serialize/serialize.h"
#include "definition_validator/validator.h"
#include "infer/infer.h"
#include "local_vars/local_vars.h"
#include "main/autogen/autogen.h"
#include "main/autogen/crc_builder.h"
#include "main/autogen/data/version.h"
#include "namer/namer.h"
#include "packager/packager.h"
#include "parser/parser.h"
#include "payload/binary/binary.h"
#include "resolver/resolver.h"
#include "rewriter/rewriter.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include "test/helpers/expectations.h"
#include "test/helpers/position_assertions.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <vector>

namespace sorbet::test {
namespace spd = spdlog;
using namespace std;

string singleTest;

class CFGCollectorAndTyper {
public:
    vector<unique_ptr<cfg::CFG>> cfgs;
    ast::ExpressionPtr preTransformMethodDef(core::Context ctx, ast::ExpressionPtr tree) {
        auto &m = ast::cast_tree_nonnull<ast::MethodDef>(tree);

        if (m.symbol.data(ctx)->isOverloaded()) {
            return tree;
        }
        auto cfg = cfg::CFGBuilder::buildFor(ctx.withOwner(m.symbol), m);
        auto symbol = cfg->symbol;
        cfg = infer::Inference::run(ctx.withOwner(symbol), move(cfg));
        if (cfg) {
            for (auto &extension : ctx.state.semanticExtensions) {
                extension->typecheck(ctx, *cfg, m);
            }
        }
        cfgs.push_back(move(cfg));
        return tree;
    }
};

UnorderedSet<string> knownExpectations = {"parse-tree",       "parse-tree-json",  "parse-tree-whitequark",
                                          "desugar-tree",     "desugar-tree-raw", "rewrite-tree",
                                          "rewrite-tree-raw", "index-tree",       "index-tree-raw",
                                          "symbol-table",     "symbol-table-raw", "name-tree",
                                          "name-tree-raw",    "resolve-tree",     "resolve-tree-raw",
                                          "flatten-tree",     "flatten-tree-raw", "cfg",
                                          "cfg-raw",          "cfg-text",         "autogen",
                                          "document-symbols", "package-tree",     "document-formatting-rubyfmt",
                                          "autocorrects"};

ast::ParsedFile testSerialize(core::GlobalState &gs, ast::ParsedFile expr) {
    auto &savedFile = expr.file.data(gs);
    auto saved = core::serialize::Serializer::storeTree(savedFile, expr);
    auto restored = core::serialize::Serializer::loadTree(gs, savedFile, saved.data());
    return {move(restored), expr.file};
}

/** Converts a Sorbet Error object into an equivalent LSP Diagnostic object. */
unique_ptr<Diagnostic> errorToDiagnostic(const core::GlobalState &gs, const core::Error &error) {
    if (!error.loc.exists()) {
        return nullptr;
    }
    return make_unique<Diagnostic>(Range::fromLoc(gs, error.loc), error.header);
}

class ExpectationHandler {
    Expectations &test;
    shared_ptr<core::ErrorQueue> &errorQueue;
    shared_ptr<core::ErrorCollector> &errorCollector;

public:
    vector<unique_ptr<core::Error>> errors;
    UnorderedMap<string_view, string> got;

    ExpectationHandler(Expectations &test, shared_ptr<core::ErrorQueue> &errorQueue,
                       shared_ptr<core::ErrorCollector> &errorCollector)
        : test(test), errorQueue(errorQueue), errorCollector(errorCollector){};

    void addObserved(const core::GlobalState &gs, string_view expectationType, std::function<string()> mkExp,
                     bool addNewline = true) {
        if (test.expectations.contains(expectationType)) {
            got[expectationType].append(mkExp());
            if (addNewline) {
                got[expectationType].append("\n");
            }
            drainErrors(gs);
        }
    }

    void checkExpectations(string prefix = "") {
        for (auto &gotPhase : got) {
            auto expectation = test.expectations.find(gotPhase.first);
            REQUIRE_MESSAGE(expectation != test.expectations.end(),
                            prefix << "missing expectation for " << gotPhase.first);
            REQUIRE_MESSAGE(expectation->second.size() == 1,
                            prefix << "found unexpected multiple expectations of type " << gotPhase.first);

            auto checker = test.folder + expectation->second.begin()->second;
            auto expect = FileOps::read(checker);

            CHECK_EQ_DIFF(expect, gotPhase.second, fmt::format("{}Mismatch on: {}", prefix, checker));
            if (expect == gotPhase.second) {
                MESSAGE(gotPhase.first << " OK");
            }
        }
    }

    void drainErrors(const core::GlobalState &gs) {
        errorQueue->flushAllErrors(gs);
        auto newErrors = errorCollector->drainErrors();
        errors.insert(errors.end(), make_move_iterator(newErrors.begin()), make_move_iterator(newErrors.end()));
    }

    void clear(const core::GlobalState &gs) {
        got.clear();
        errorQueue->flushAllErrors(gs);
        errorCollector->drainErrors();
    }
};

TEST_CASE("PerPhaseTest") { // NOLINT
    Expectations test = Expectations::getExpectations(singleTest);

    auto inputPath = test.folder + test.basename;
    auto rbName = test.basename + ".rb";

    for (auto &exp : test.expectations) {
        if (!knownExpectations.contains(exp.first)) {
            FAIL_CHECK("Unknown pass: " << exp.first);
        }
    }

    auto logger = spd::stderr_color_mt("fixtures: " + inputPath);
    auto errorCollector = make_shared<core::ErrorCollector>();
    auto errorQueue = make_shared<core::ErrorQueue>(*logger, *logger, errorCollector);
    auto gs = make_unique<core::GlobalState>(errorQueue);

    for (auto provider : sorbet::pipeline::semantic_extension::SemanticExtensionProvider::getProviders()) {
        gs->semanticExtensions.emplace_back(provider->defaultInstance());
    }

    gs->censorForSnapshotTests = true;
    auto workers = WorkerPool::create(0, gs->tracer());

    auto assertions = RangeAssertion::parseAssertions(test.sourceFileContents);

    gs->requiresAncestorEnabled =
        BooleanPropertyAssertion::getValue("enable-experimental-requires-ancestor", assertions).value_or(false);

    if (BooleanPropertyAssertion::getValue("no-stdlib", assertions).value_or(false)) {
        gs->initEmpty();
    } else {
        core::serialize::Serializer::loadGlobalState(*gs, getNameTablePayload);
    }

    if (BooleanPropertyAssertion::getValue("enable-suggest-unsafe", assertions).value_or(false)) {
        gs->suggestUnsafe = "T.unsafe";
    }

    // Parser
    vector<core::FileRef> files;
    constexpr string_view whitelistedTypedNoneTest = "missing_typed_sigil.rb"sv;
    constexpr string_view packageFileName = "__package.rb"sv;
    {
        core::UnfreezeFileTable fileTableAccess(*gs);

        for (auto &sourceFile : test.sourceFiles) {
            auto fref = gs->enterFile(test.sourceFileContents[test.folder + sourceFile]);
            if (FileOps::getFileName(sourceFile) == whitelistedTypedNoneTest) {
                fref.data(*gs).strictLevel = core::StrictLevel::False;
            }
            if (FileOps::getFileName(sourceFile) == packageFileName && fref.data(*gs).source().empty()) {
                fref.data(*gs).strictLevel = core::StrictLevel::False;
            }
            files.emplace_back(fref);
        }
    }
    vector<ast::ParsedFile> trees;
    ExpectationHandler handler(test, errorQueue, errorCollector);

    for (auto file : files) {
        auto fileName = FileOps::getFileName(file.data(*gs).path());
        if (fileName != whitelistedTypedNoneTest && (fileName != packageFileName || !file.data(*gs).source().empty()) &&
            file.data(*gs).source().find("# typed:") == string::npos) {
            ADD_FAIL_CHECK_AT(file.data(*gs).path().data(), 1, "Add a `# typed: strict` line to the top of this file");
        }
        unique_ptr<parser::Node> nodes;
        {
            core::UnfreezeNameTable nameTableAccess(*gs); // enters original strings

            nodes = parser::Parser::run(*gs, file);
        }

        handler.drainErrors(*gs);
        handler.addObserved(*gs, "parse-tree", [&]() { return nodes->toString(*gs); });
        handler.addObserved(*gs, "parse-tree-whitequark", [&]() { return nodes->toWhitequark(*gs); });
        handler.addObserved(*gs, "parse-tree-json", [&]() { return nodes->toJSON(*gs); });

        // Desugarer
        ast::ParsedFile desugared;
        {
            core::UnfreezeNameTable nameTableAccess(*gs); // enters original strings

            core::MutableContext ctx(*gs, core::Symbols::root(), file);
            desugared = testSerialize(*gs, ast::ParsedFile{ast::desugar::node2Tree(ctx, move(nodes)), file});
        }

        handler.addObserved(*gs, "desugar-tree", [&]() { return desugared.tree.toString(*gs); });
        handler.addObserved(*gs, "desugar-tree-raw", [&]() { return desugared.tree.showRaw(*gs); });

        ast::ParsedFile localNamed;

        if (!test.expectations.contains("autogen")) {
            // Rewriter
            ast::ParsedFile rewriten;
            {
                core::UnfreezeNameTable nameTableAccess(*gs); // enters original strings

                core::MutableContext ctx(*gs, core::Symbols::root(), desugared.file);
                rewriten = testSerialize(
                    *gs, ast::ParsedFile{rewriter::Rewriter::run(ctx, move(desugared.tree)), desugared.file});
            }

            handler.addObserved(*gs, "rewrite-tree", [&]() { return rewriten.tree.toString(*gs); });
            handler.addObserved(*gs, "rewrite-tree-raw", [&]() { return rewriten.tree.showRaw(*gs); });

            core::MutableContext ctx(*gs, core::Symbols::root(), desugared.file);
            localNamed = testSerialize(*gs, local_vars::LocalVars::run(ctx, move(rewriten)));

            handler.addObserved(*gs, "index-tree", [&]() { return localNamed.tree.toString(*gs); });
            handler.addObserved(*gs, "index-tree-raw", [&]() { return localNamed.tree.showRaw(*gs); });
        } else {
            core::MutableContext ctx(*gs, core::Symbols::root(), desugared.file);
            localNamed = testSerialize(*gs, local_vars::LocalVars::run(ctx, move(desugared)));
            if (test.expectations.contains("rewrite-tree-raw") || test.expectations.contains("rewrite-tree")) {
                FAIL_CHECK("Running Rewriter passes with autogen isn't supported");
            }
        }
        trees.emplace_back(move(localNamed));
    }

    auto enablePackager = BooleanPropertyAssertion::getValue("enable-packager", assertions).value_or(false);
    vector<std::string> extraPackageFilesDirectoryPrefixes;
    if (enablePackager) {
        auto extraDir = StringPropertyAssertion::getValue("extra-package-files-directory-prefix", assertions);
        if (extraDir.has_value()) {
            extraPackageFilesDirectoryPrefixes.emplace_back(extraDir.value());
        }

        // Packager runs over all trees.
        trees = packager::Packager::run(*gs, *workers, move(trees), extraPackageFilesDirectoryPrefixes);
        for (auto &tree : trees) {
            handler.addObserved(*gs, "package-tree", [&]() { return tree.tree.toString(*gs); });
        }
    }

    for (auto &tree : trees) {
        // Namer
        ast::ParsedFile namedTree;
        {
            core::UnfreezeNameTable nameTableAccess(*gs);     // creates singletons and class names
            core::UnfreezeSymbolTable symbolTableAccess(*gs); // enters symbols
            vector<ast::ParsedFile> vTmp;
            vTmp.emplace_back(move(tree));
            vTmp = move(namer::Namer::run(*gs, move(vTmp), *workers).result());
            namedTree = testSerialize(*gs, move(vTmp[0]));
        }

        handler.addObserved(*gs, "name-tree", [&]() { return namedTree.tree.toString(*gs); });
        handler.addObserved(*gs, "name-tree-raw", [&]() { return namedTree.tree.showRaw(*gs); });

        tree = move(namedTree);
    }

    if (test.expectations.contains("autogen")) {
        {
            core::UnfreezeNameTable nameTableAccess(*gs);
            core::UnfreezeSymbolTable symbolAccess(*gs);

            trees = resolver::Resolver::runConstantResolution(*gs, move(trees), *workers);
        }
        handler.addObserved(
            *gs, "autogen",
            [&]() {
                stringstream payload;
                auto crcBuilder = autogen::CRCBuilder::create();
                for (auto &tree : trees) {
                    core::Context ctx(*gs, core::Symbols::root(), tree.file);
                    auto pf = autogen::Autogen::generate(ctx, move(tree), *crcBuilder);
                    tree = move(pf.tree);
                    payload << pf.toString(ctx, autogen::AutogenVersion::MAX_VERSION);
                }
                return payload.str();
            },
            false);

        handler.checkExpectations();

        // Autogen forces you to to put --stop-after=namer so lets not run
        // anything else
        return;
    } else {
        core::UnfreezeNameTable nameTableAccess(*gs);     // Resolver::defineAttr
        core::UnfreezeSymbolTable symbolTableAccess(*gs); // enters stubs
        trees = move(resolver::Resolver::run(*gs, move(trees), *workers).result());
        handler.drainErrors(*gs);
    }

    handler.addObserved(*gs, "symbol-table", [&]() { return gs->toString(); });
    handler.addObserved(*gs, "symbol-table-raw", [&]() { return gs->showRaw(); });

    for (auto &resolvedTree : trees) {
        handler.addObserved(*gs, "resolve-tree", [&]() { return resolvedTree.tree.toString(*gs); });
        handler.addObserved(*gs, "resolve-tree-raw", [&]() { return resolvedTree.tree.showRaw(*gs); });
    }

    // Simulate what pipeline.cc does: We want to start typeckecking big files first because it helps with better work
    // distribution
    fast_sort(trees, [&](const auto &lhs, const auto &rhs) -> bool {
        return lhs.file.data(*gs).source().size() > rhs.file.data(*gs).source().size();
    });

    for (auto &resolvedTree : trees) {
        auto file = resolvedTree.file;

        core::Context ctx(*gs, core::Symbols::root(), file);
        resolvedTree = definition_validator::runOne(ctx, move(resolvedTree));
        handler.drainErrors(*gs);

        resolvedTree = class_flatten::runOne(ctx, move(resolvedTree));

        handler.addObserved(*gs, "flatten-tree", [&]() { return resolvedTree.tree.toString(*gs); });
        handler.addObserved(*gs, "flatten-tree-raw", [&]() { return resolvedTree.tree.showRaw(*gs); });

        // Don't run typecheck on RBI files.
        if (resolvedTree.file.data(ctx).isRBI()) {
            continue;
        }

        auto checkTree = [&]() {
            if (resolvedTree.tree == nullptr) {
                auto path = file.data(*gs).path();
                ADD_FAIL_CHECK_AT(path.begin(), 1, "Already used tree. You can only have 1 CFG-ish .exp file");
            }
        };
        auto checkPragma = [&](string ext) {
            if (file.data(*gs).strictLevel < core::StrictLevel::True) {
                auto path = file.data(*gs).path();
                ADD_FAIL_CHECK_AT(path.begin(), 1,
                                  "Missing `# typed:` pragma. Sources with ." << ext
                                                                              << ".exp files must specify # typed:");
            }
        };

        // CFG
        if (test.expectations.contains("cfg") || test.expectations.contains("cfg-raw")) {
            checkTree();
            checkPragma("cfg");
            CFGCollectorAndTyper collector;
            core::Context ctx(*gs, core::Symbols::root(), resolvedTree.file);
            auto cfg = ast::TreeMap::apply(ctx, collector, move(resolvedTree.tree));
            for (auto &extension : ctx.state.semanticExtensions) {
                extension->finishTypecheckFile(ctx, file);
            }
            resolvedTree.tree.reset();

            handler.addObserved(*gs, "cfg", [&]() {
                stringstream dot;
                dot << "digraph \"" << rbName << "\" {" << '\n';
                for (auto &cfg : collector.cfgs) {
                    dot << cfg->toString(ctx) << '\n' << '\n';
                }
                dot << "}" << '\n';
                return dot.str();
            });

            handler.addObserved(*gs, "cfg-raw", [&]() {
                stringstream dot;
                dot << "digraph \"" << rbName << "\" {" << '\n';
                dot << "  graph [fontname = \"Courier\"];\n";
                dot << "  node [fontname = \"Courier\"];\n";
                dot << "  edge [fontname = \"Courier\"];\n";
                for (auto &cfg : collector.cfgs) {
                    dot << cfg->showRaw(ctx) << '\n' << '\n';
                }
                dot << "}" << '\n';
                return dot.str();
            });
        }

        // If there is a tree left with a typed: pragma, run the inferencer
        if (resolvedTree.tree != nullptr && file.data(*gs).originalSigil >= core::StrictLevel::True) {
            checkTree();
            CFGCollectorAndTyper collector;
            core::Context ctx(*gs, core::Symbols::root(), resolvedTree.file);
            ast::TreeMap::apply(ctx, collector, move(resolvedTree.tree));
            for (auto &extension : ctx.state.semanticExtensions) {
                extension->finishTypecheckFile(ctx, file);
            }
            resolvedTree.tree.reset();
            handler.drainErrors(*gs);
        }
    }

    for (auto &extension : gs->semanticExtensions) {
        extension->finishTypecheck(*gs);
    }

    handler.checkExpectations();

    if (test.expectations.contains("symbol-table")) {
        string table = gs->toString() + '\n';
        CHECK_EQ_DIFF(handler.got["symbol-table"], table, "symbol-table should not be mutated by CFG+inference");
    }

    if (test.expectations.contains("symbol-table-raw")) {
        string table = gs->showRaw() + '\n';
        CHECK_EQ_DIFF(handler.got["symbol-table-raw"], table,
                      "symbol-table-raw should not be mutated by CFG+inference");
    }

    // Check warnings and errors
    {
        map<string, vector<unique_ptr<Diagnostic>>> diagnostics;
        for (auto &error : handler.errors) {
            if (error->isSilenced) {
                continue;
            }
            auto diag = errorToDiagnostic(*gs, *error);
            ENFORCE(diag != nullptr, "Error was given no valid location - '{}'", error->toString(*gs));

            auto path = error->loc.file().data(*gs).path();
            diagnostics[string(path.begin(), path.end())].push_back(std::move(diag));
        }
        ErrorAssertion::checkAll(test.sourceFileContents, RangeAssertion::getErrorAssertions(assertions), diagnostics);
    }

    // Check autocorrects
    {
        auto autocorrects = vector<core::AutocorrectSuggestion>{};
        for (const auto &error : handler.errors) {
            if (error->isSilenced) {
                continue;
            }

            for (const auto &autocorrect : error->autocorrects) {
                autocorrects.push_back(autocorrect);
            }
        }

        auto fs = OSFileSystem{};
        auto applied = core::AutocorrectSuggestion::apply(*gs, fs, autocorrects);

        auto toWrite = vector<pair<core::FileRef, string>>{};
        for (const auto &[file, editedSource] : applied) {
            toWrite.emplace_back(file, move(editedSource));
        }

        fast_sort(toWrite, [&](const auto &lhs, const auto &rhs) {
            return lhs.first.data(*gs).path() < rhs.first.data(*gs).path();
        });

        auto addNewline = false;
        handler.addObserved(
            *gs, "autocorrects",
            [&]() {
                fmt::memory_buffer buf;
                for (const auto &[file, editedSource] : toWrite) {
                    fmt::format_to(std::back_inserter(buf), "# -- {} --\n{}# ------------------------------\n",
                                   core::File::censorFilePathForSnapshotTests(file.data(*gs).path()), editedSource);
                }
                return to_string(buf);
            },
            addNewline);
    }

    handler.checkExpectations();

    // Allow later phases to have errors that we didn't test for
    errorQueue->flushAllErrors(*gs);
    errorCollector->drainErrors();

    // now we test the incremental resolver

    auto disableStressIncremental =
        BooleanPropertyAssertion::getValue("disable-stress-incremental", assertions).value_or(false);
    if (disableStressIncremental) {
        MESSAGE("errors OK");
        return;
    }

    handler.clear(*gs);
    auto symbolsBefore = gs->symbolsUsedTotal();

    vector<ast::ParsedFile> newTrees;
    for (auto &f : trees) {
        const int prohibitedLines = f.file.data(*gs).source().size();
        auto newSource = absl::StrCat(string(prohibitedLines + 1, '\n'), f.file.data(*gs).source());
        auto newFile =
            make_shared<core::File>(string(f.file.data(*gs).path()), move(newSource), f.file.data(*gs).sourceType);
        gs = core::GlobalState::replaceFile(move(gs), f.file, move(newFile));

        // this replicates the logic of pipeline::indexOne
        auto nodes = parser::Parser::run(*gs, f.file);
        handler.addObserved(*gs, "parse-tree", [&]() { return nodes->toString(*gs); });
        handler.addObserved(*gs, "parse-tree-json", [&]() { return nodes->toJSON(*gs); });

        core::MutableContext ctx(*gs, core::Symbols::root(), f.file);
        ast::ParsedFile file = testSerialize(*gs, ast::ParsedFile{ast::desugar::node2Tree(ctx, move(nodes)), f.file});
        handler.addObserved(*gs, "desguar-tree", [&]() { return file.tree.toString(*gs); });
        handler.addObserved(*gs, "desugar-tree-raw", [&]() { return file.tree.showRaw(*gs); });

        // Rewriter pass
        file = testSerialize(*gs, ast::ParsedFile{rewriter::Rewriter::run(ctx, move(file.tree)), file.file});
        handler.addObserved(*gs, "rewrite-tree", [&]() { return file.tree.toString(*gs); });
        handler.addObserved(*gs, "rewrite-tree-raw", [&]() { return file.tree.showRaw(*gs); });

        // local vars
        file = testSerialize(*gs, local_vars::LocalVars::run(ctx, move(file)));
        handler.addObserved(*gs, "index-tree", [&]() { return file.tree.toString(*gs); });
        handler.addObserved(*gs, "index-tree-raw", [&]() { return file.tree.showRaw(*gs); });

        newTrees.emplace_back(move(file));
    }

    trees = move(newTrees);
    if (enablePackager) {
        trees = packager::Packager::runIncremental(*gs, move(trees), extraPackageFilesDirectoryPrefixes);
        for (auto &tree : trees) {
            handler.addObserved(*gs, "package-tree", [&]() { return tree.tree.toString(*gs); });
        }
    }

    {
        // namer
        for (auto &tree : trees) {
            core::UnfreezeSymbolTable symbolTableAccess(*gs);
            vector<ast::ParsedFile> vTmp;
            vTmp.emplace_back(move(tree));
            vTmp = move(namer::Namer::run(*gs, move(vTmp), *workers).result());
            tree = testSerialize(*gs, move(vTmp[0]));

            handler.addObserved(*gs, "name-tree", [&]() { return tree.tree.toString(*gs); });
            handler.addObserved(*gs, "name-tree-raw", [&]() { return tree.tree.showRaw(*gs); });
        }
    }

    // resolver
    trees = move(resolver::Resolver::runIncremental(*gs, move(trees)).result());

    for (auto &resolvedTree : trees) {
        handler.addObserved(*gs, "resolve-tree", [&]() { return resolvedTree.tree.toString(*gs); });
        handler.addObserved(*gs, "resolve-tree-raw", [&]() { return resolvedTree.tree.showRaw(*gs); });
    }

    handler.checkExpectations("[stress-incremental] ");

    // and drain all the remaining errors
    errorQueue->flushAllErrors(*gs);
    errorCollector->drainErrors();

    {
        INFO("the incremental resolver should not add new symbols");
        CHECK_EQ(symbolsBefore, gs->symbolsUsedTotal());
    }
}
} // namespace sorbet::test

int main(int argc, char *argv[]) {
    cxxopts::Options options("test_corpus", "Test corpus for Sorbet typechecker");
    options.allow_unrecognised_options().add_options()("single_test", "run over single test.",
                                                       cxxopts::value<std::string>()->default_value(""), "testpath");
    auto res = options.parse(argc, argv);

    if (res.count("single_test") != 1) {
        printf("--single_test=<filename> argument expected\n");
        return 1;
    }

    sorbet::test::singleTest = res["single_test"].as<std::string>();

    doctest::Context context(argc, argv);
    return context.run();
}
