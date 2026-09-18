//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE PipeRegistry
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include "revng/PipeboxCommon/Helpers/Native/Registry.h"

using namespace revng::pypeline::helpers::native;

// Registration happens from static initialisers in `revngPipebox`, and nothing
// here refers to a symbol in it, so the library has to be retained explicitly
// by the build. If that ever stops happening these registries are empty and
// every lookup fails at run time with nothing to point at the cause.
BOOST_AUTO_TEST_CASE(RegistriesArePopulated) {
  BOOST_CHECK(not Registry.Pipes.empty());
  BOOST_CHECK(not Registry.Analyses.empty());
  BOOST_CHECK(not Registry.Containers.empty());
}

// A representative slice of the pipeline: if these resolve, registration is
// reaching the native registry rather than only the Python one.
BOOST_AUTO_TEST_CASE(KnownPipesAreRegistered) {
  for (llvm::StringRef Name : { "lift", "isolate", "emit-c" })
    BOOST_CHECK_MESSAGE(Registry.Pipes.count(Name) == 1,
                        "pipe not registered: " << Name.str());

  for (llvm::StringRef Name : { "detect-abi", "analyze-data-layout" })
    BOOST_CHECK_MESSAGE(Registry.Analyses.count(Name) == 1,
                        "analysis not registered: " << Name.str());
}
