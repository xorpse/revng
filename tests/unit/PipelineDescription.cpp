//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE PipelineDescription
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include "revng/PipeboxCommon/Helpers/Native/Registry.h"
#include "revng/Runner/PipelineDescription.h"
#include "revng/Support/Assert.h"

using namespace revng::runner;

static PipelineDescription parseShipped() {
  auto Parsed = PipelineDescription::fromFile(REVNG_PIPELINE_PATH);
  BOOST_REQUIRE_MESSAGE(bool(Parsed),
                        "failed to parse " << REVNG_PIPELINE_PATH << ": "
                                           << toString(Parsed.takeError()));
  return std::move(*Parsed);
}

BOOST_AUTO_TEST_CASE(ParsesTheShippedPipeline) {
  PipelineDescription Description = parseShipped();

  BOOST_CHECK(not Description.Declarations.empty());
  BOOST_CHECK(not Description.Nodes.empty());

  // Containers the embedding API addresses by name.
  for (llvm::StringRef Name :
       { "binaries-container", "llvm-root", "llvm-functions", "decompile-c" })
    BOOST_CHECK_MESSAGE(Description.findDeclaration(Name).has_value(),
                        "missing container: " << Name.str());

  BOOST_CHECK_EQUAL(Description.Declarations[*Description
                                                .findDeclaration("llvm-root")]
                      .TypeName,
                    "LLVMRootContainer");
}

BOOST_AUTO_TEST_CASE(ResolvesSavepointsAndArtifacts) {
  PipelineDescription Description = parseShipped();

  for (llvm::StringRef Name : { "lifted", "segregate-stack-accesses" })
    BOOST_CHECK_MESSAGE(Description.findSavepoint(Name) != nullptr,
                        "missing savepoint: " << Name.str());

  for (llvm::StringRef Name : { "lift", "emit-c" })
    BOOST_CHECK_MESSAGE(Description.findArtifact(Name) != nullptr,
                        "missing artifact: " << Name.str());

  // An artifact names both the node that produced it and the container to read.
  const Artifact *Lift = Description.findArtifact("lift");
  BOOST_REQUIRE(Lift != nullptr);
  BOOST_CHECK(Lift->Node != nullptr);
  BOOST_CHECK_EQUAL(Description.Declarations[Lift->Declaration].Name,
                    "llvm-root");
}

BOOST_AUTO_TEST_CASE(BindsAnalyses) {
  PipelineDescription Description = parseShipped();

  // Bound to the node whose containers they read.
  for (llvm::StringRef Name : { "detect-abi", "analyze-data-layout" }) {
    const AnalysisBinding *Binding = Description.findAnalysis(Name);
    BOOST_REQUIRE_MESSAGE(Binding != nullptr,
                          "missing analysis: " << Name.str());
    BOOST_CHECK(Binding->Node != nullptr);
    BOOST_CHECK(not Binding->Arguments.empty());
  }

  // Declared at the top level, so it reads no container and runs at the root.
  const AnalysisBinding *ToCABI = Description
                                    .findAnalysis("convert-functions-to-cabi");
  BOOST_REQUIRE(ToCABI != nullptr);
  BOOST_CHECK(ToCABI->Node == nullptr);

  BOOST_CHECK(Description.AnalysisLists.count("initial-auto-analysis") == 1);
}

BOOST_AUTO_TEST_CASE(BranchesFormATree) {
  PipelineDescription Description = parseShipped();

  // Branches form a forest, not a single tree: `lift` and `import-types` both
  // start from nothing. What the scheduler relies on is that every node has at
  // most one predecessor, so walking backwards from a target is unambiguous.
  size_t Roots = 0;
  for (const std::unique_ptr<PipelineNode> &Node : Description.Nodes) {
    if (Node->Predecessor == nullptr) {
      ++Roots;
    } else {
      BOOST_CHECK(llvm::is_contained(Node->Predecessor->Successors, Node.get()));
      BOOST_CHECK(Node->Predecessor != Node.get());
    }
  }

  BOOST_CHECK(Roots >= 1U);

  // Walking back from any node terminates.
  for (const std::unique_ptr<PipelineNode> &Node : Description.Nodes) {
    size_t Steps = 0;
    for (const PipelineNode *Current = Node.get(); Current != nullptr;
         Current = Current->Predecessor) {
      BOOST_REQUIRE_LT(Steps, Description.Nodes.size());
      ++Steps;
    }
  }
}

// A standing check that the description and the code have not drifted apart:
// every pipe and analysis the pipeline names must actually be registered, or
// running it fails at the point of use with nothing to point at the cause.
BOOST_AUTO_TEST_CASE(EveryNamedPipeAndAnalysisIsRegistered) {
  using namespace revng::pypeline::helpers::native;
  PipelineDescription Description = parseShipped();

  for (const std::unique_ptr<PipelineNode> &Node : Description.Nodes) {
    if (Node->isSavepoint())
      continue;

    llvm::StringRef Name = Node->pipe().PipeName;
    BOOST_CHECK_MESSAGE(Registry.Pipes.count(Name) == 1,
                        "pipeline names an unregistered pipe: " << Name.str());
  }

  for (const auto &Entry : Description.Analyses) {
    llvm::StringRef Name = Entry.first();
    BOOST_CHECK_MESSAGE(Registry.Analyses.count(Name) == 1,
                        "pipeline names an unregistered analysis: "
                          << Name.str());
  }

  for (const ContainerDeclaration &Declaration : Description.Declarations) {
    llvm::StringRef Name = Declaration.TypeName;
    BOOST_CHECK_MESSAGE(Registry.Containers.count(Name) == 1,
                        "pipeline names an unregistered container type: "
                          << Name.str());
  }
}

// `arguments: [llvm-root]` is a flow sequence, and several tasks index their
// arguments positionally without a bounds check, so a task parsed with no
// arguments crashes rather than failing.
BOOST_AUTO_TEST_CASE(EveryPipeHasItsArguments) {
  PipelineDescription Description = parseShipped();

  for (const std::unique_ptr<PipelineNode> &Node : Description.Nodes) {
    if (Node->isSavepoint())
      continue;

    BOOST_CHECK_MESSAGE(not Node->Arguments.empty(),
                        "pipe parsed with no arguments: "
                          << Node->pipe().PipeName);
  }
}

// The lift-only pipeline the C API tests and the installed-SDK examples use.
BOOST_AUTO_TEST_CASE(ParsesTheAddressSpacePipeline) {
  auto Parsed = PipelineDescription::fromFile(REVNG_ADDRESS_SPACE_PIPELINE);
  BOOST_REQUIRE_MESSAGE(bool(Parsed),
                        "failed to parse "
                          << REVNG_ADDRESS_SPACE_PIPELINE << ": "
                          << toString(Parsed.takeError()));

  BOOST_CHECK(Parsed->findDeclaration("llvm-root").has_value());
  BOOST_CHECK(Parsed->findDeclaration("binaries-container").has_value());
  BOOST_CHECK(Parsed->findSavepoint("lifted") != nullptr);
  BOOST_CHECK(Parsed->findArtifact("lift") != nullptr);
}
