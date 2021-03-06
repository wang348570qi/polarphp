// This source file is part of the polarphp.org open source project
//
// Copyright (c) 2017 - 2018 polarphp software foundation
// Copyright (c) 2017 - 2018 zzu_softboy <zzu_softboy@163.com>
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://polarphp.org/LICENSE.txt for license information
// See http://polarphp.org/CONTRIBUTORS.txt for the list of polarphp project authors
//
// Created by polarboy on 2018/09/05.

#include "Discovery.h"
#include "LitConfig.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "formats/Base.h"
#include "Run.h"
#include "LitTestCase.h"

namespace fs = std::filesystem;

namespace polar {
namespace lit {

using StringMap = std::map<std::string, std::string>;

std::optional<std::string> choose_config_file_from_dir(const std::string &dir,
                                                       const std::list<std::string> &configNames)
{
   for (const std::string &name : configNames) {
      fs::path p(dir);
      p /= name;
      if (fs::exists(p)) {
         return p;
      }
   }
   return std::nullopt;
}

std::optional<std::string> dir_contains_test_suite(const std::string &path,
                                                   LitConfigPointer litConfig)
{
   std::optional<std::string> cfgPath = choose_config_file_from_dir(path, litConfig->getSiteConfigNames());
   if (!cfgPath.has_value()) {
      cfgPath = choose_config_file_from_dir(path, litConfig->getConfigNames());
   }
   return cfgPath;
}

namespace {

TestSuitSearchResult search_testsuit(const std::string &path,
                                     LitConfigPointer litConfig,
                                     std::map<std::string, TestSuitSearchResult> &cache);

TestSuitSearchResult do_search_testsuit(const std::string &path,
                                        LitConfigPointer litConfig,
                                        std::map<std::string, TestSuitSearchResult> &cache)
{
   // Check for a site config or a lit config.
   std::optional<std::string> cfgPathOpt = dir_contains_test_suite(path, litConfig);
   // If we didn't find a config file, keep looking.
   std::string base;
   if (!cfgPathOpt.has_value()) {
      fs::path fsPath(path);
      std::string parent = fsPath.parent_path();
      base = fsPath.filename();
      if (parent == path) {
         return TestSuitSearchResult{std::nullopt, std::list<std::string>{}};
      }
      TestSuitSearchResult temp = search_testsuit(parent, litConfig, cache);
      std::get<1>(temp).push_back(base);
      return temp;
   }
   // This is a private builtin parameter which can be used to perform
   // translation of configuration paths.  Specifically, this parameter
   // can be set to a dictionary that the discovery process will consult
   // when it finds a configuration it is about to load.  If the given
   // path is in the map, the value of that key is a path to the
   // configuration to load instead.
   std::any configMapAny = litConfig->getParams().at("config_map");
   std::string cfgPath;
   if (configMapAny.has_value()) {
      cfgPath = fs::canonical(cfgPathOpt.value());
      StringMap &configMap = std::any_cast<StringMap&>(configMapAny);
      if (configMap.find(cfgPath) != configMap.end()) {
         cfgPath = configMap.at(cfgPath);
      }
   }
   // We found a test suite, create a new config for it and load it.
   if (litConfig->isDebug()) {
      litConfig->note(format_string("loading suite config %s", cfgPath.c_str()), __FILE__, __LINE__);
   }
   TestingConfigPointer testingCfg = TestingConfig::fromDefaults(litConfig);
   testingCfg->loadFromPath(cfgPath, *litConfig.get());
   std::string sourceRoot;
   std::string execRoot;
   if (testingCfg->getTestSourceRoot().has_value()) {
      sourceRoot = testingCfg->getTestSourceRoot().value();
   } else {
      sourceRoot = path;
   }
   if (testingCfg->getTestExecRoot().has_value()) {
      execRoot = testingCfg->getTestExecRoot().value();
   } else {
      execRoot = path;
   }
   return TestSuitSearchResult{std::make_shared<TestSuite>(testingCfg->getName(), sourceRoot, execRoot, testingCfg),
            std::list<std::string>{}};
}

TestSuitSearchResult search_testsuit(const std::string &path,
                                     LitConfigPointer litConfig,
                                     std::map<std::string, TestSuitSearchResult> &cache)
{
   // Check for an already instantiated test suite.
   std::string realPath = fs::canonical(path);
   if (cache.find(realPath) == cache.end()) {
      cache[realPath] = do_search_testsuit(path, litConfig, cache);
   }
   return cache.at(realPath);
}

} // anonymous namespace

/// getTestSuite(item, litConfig, cache) -> (suite, relative_path)
/// Find the test suite containing @arg item.
/// @retval (None, ...) - Indicates no test suite contains @arg item.
/// @retval (suite, relative_path) - The suite that @arg item is in, and its
/// relative path inside that suite.
TestSuitSearchResult get_test_suite(std::string item, LitConfigPointer litConfig,
                                    std::map<std::string, TestSuitSearchResult> &cache)
{
   // Canonicalize the path.
   item = fs::canonical(fs::current_path() / item);
   // Skip files and virtual components.
   std::list<std::string> components;
   fs::path currentDir(item);
   while (!fs::is_directory(currentDir)) {
      std::string parent = currentDir.parent_path();
      std::string base = currentDir.filename();
      if (parent == currentDir.string()) {
         return TestSuitSearchResult{std::nullopt, std::list<std::string>{}};
      }
      components.push_back(base);
      currentDir = parent;
   }
   components.reverse();
   TestSuitSearchResult temp = search_testsuit(currentDir.string(), litConfig, cache);
   for (const std::string &item : components) {
      std::get<1>(temp).push_back(item);
   }
   return temp;
}

TestingConfigPointer get_local_config(TestSuitePointer testSuite, LitConfigPointer litConfig,
                                      const std::list<std::string> &pathInSuite)
{
   TestingConfigPointer parent;
   if (pathInSuite.empty()) {
      parent = testSuite->getConfig();
   } else {
      std::list<std::string> paths = pathInSuite;
      paths.pop_back();
      parent = get_local_config(testSuite, litConfig, paths);
   }
   std::string sourcePath = testSuite->getSourcePath(pathInSuite);
   std::optional<std::string> cfgPath = choose_config_file_from_dir(sourcePath, litConfig->getLocalConfigNames());
   // If not, just reuse the parent config.
   if (!cfgPath.has_value()){
      return parent;
   }
   // Otherwise, copy the current config and load the local configuration
   // file into it.
   TestingConfigPointer config(new TestingConfig(*parent.get()));
   if (litConfig->isDebug()) {
      litConfig->note(format_string("loading local config %s", cfgPath.value().c_str()));
   }
   config->loadFromPath(cfgPath.value(), *litConfig.get());
   return config;
}

TestList get_tests_in_suite(TestSuitePointer testSuite, LitConfigPointer litConfig,
                            const std::list<std::string> &pathInSuite,
                            std::map<std::string, TestSuitSearchResult> &cache)
{
   // Check that the source path exists (errors here are reported by the
   // caller).
   std::string sourcePath = testSuite->getSourcePath(pathInSuite);
   if (!fs::exists(sourcePath)) {
      return TestList{};
   }
   // Check if the user named a test directly.
   if (!fs::is_directory(sourcePath)) {
      std::list<std::string> temp = pathInSuite;
      temp.pop_back();
      TestingConfigPointer lc = get_local_config(testSuite, litConfig, temp);
      return TestList{std::make_shared<Test>(testSuite, temp, lc)};
   }
   // Search for tests.
   // @TODO use test format
   // Otherwise we have a directory to search for tests, start by getting the
   // local configuration.
   TestingConfigPointer lc = get_local_config(testSuite, litConfig, pathInSuite);
   if (lc->getTestFormat().has_value()) {
      return lc->getTestFormat().value()->getTestsInDirectory(testSuite, pathInSuite, litConfig, lc);
   }
   for(auto& p: fs::directory_iterator(sourcePath)) {
      const fs::path &path = p.path();
      std::string filename = path.filename();
      if (filename == "Output" ||
          filename == ".svn" ||
          filename == ".git" ||
          lc->getExcludes().find(filename) != lc->getExcludes().end()) {
         continue;
      }
      // Ignore non-directories.
      if (!fs::is_directory(path)) {
         continue;
      }
      // Check for nested test suites, first in the execpath in case there is a
      // site configuration and then in the source path.
      std::list<std::string> subPath = pathInSuite;
      subPath.push_back(filename);
      std::string fileExecPath = testSuite->getExecPath(subPath);
      std::optional<TestSuitePointer> subTs;
      std::list<std::string> subpathInSuite;
      if (dir_contains_test_suite(fileExecPath, litConfig).has_value()) {
         TestSuitSearchResult searchResult = get_test_suite(fileExecPath, litConfig, cache);
         subTs = std::get<0>(searchResult);
         subpathInSuite = std::get<1>(searchResult);
      } else if (dir_contains_test_suite(path.string(), litConfig).has_value()){
         TestSuitSearchResult searchResult = get_test_suite(path.string(), litConfig, cache);
         subTs = std::get<0>(searchResult);
         subpathInSuite = std::get<1>(searchResult);
      } else {
         subTs = std::nullopt;
      }
      // If the this directory recursively maps back to the current test suite,
      // disregard it (this can happen if the exec root is located inside the
      // current test suite, for example).
      if (subTs.value() == testSuite) {
         continue;
      }
      // Otherwise, load from the nested test suite, if present.
      if (subTs.has_value()) {
         return get_tests_in_suite(subTs.value(), litConfig, subpathInSuite, cache);
      } else {
         return get_tests_in_suite(testSuite, litConfig, subPath, cache);
      }
   }
}

std::tuple<TestSuitePointer, TestList> get_tests(const std::string &path, LitConfigPointer config,
                                                 std::map<std::string, TestSuitSearchResult> &cache)
{
   TestSuitSearchResult testSuiteResult = get_test_suite(path, config, cache);
   std::optional<TestSuitePointer> testSuite = std::get<0>(testSuiteResult);
   std::list<std::string> subpathInSuite = std::get<1>(testSuiteResult);
   if (!testSuite.has_value()) {
      config->warning(format_string("unable to find test suite for %s", path.c_str()));
      return std::tuple<TestSuitePointer, TestList>{};
   }
   if (config->isDebug()) {
      config->note(format_string("resolved input %s to %s", path.c_str(), testSuite.value()->getName().c_str()));
   }
   return std::tuple<TestSuitePointer, TestList>{testSuite.value(), get_tests_in_suite(testSuite.value(), config, subpathInSuite, cache)};
}


////  find_tests_for_inputs(lit_config, inputs) -> [Test]
///
/// Given a configuration object and a list of input specifiers, find all the
/// tests to execute.
std::list<std::tuple<TestSuitePointer, TestList>> find_tests_for_inputs(LitConfigPointer litConfig, const std::list<std::string> &inputs)
{
   std::list<std::string> actualInputs;
   for (const std::string &input : inputs) {
      if (string_startswith(input, "@")) {
         std::fstream f(input.substr(1));
         char lineBuf[256];
         while (f.getline(lineBuf, sizeof(lineBuf))) {
            std::string line(lineBuf);
            if (!line.empty()) {
               actualInputs.push_back(line);
            }
         }
      } else {
         actualInputs.push_back(input);
      }
   }
   std::list<std::tuple<TestSuitePointer, TestList>> tests;
   std::map<std::string, TestSuitSearchResult> cache;
   for (std::string &input : actualInputs) {
      int prevLength = tests.size();
      tests.push_back(get_tests(input, litConfig, cache));
      if (prevLength == tests.size()) {
         litConfig->warning(format_string("input %s contained no tests", input.c_str()));
      }
   }
   // If there were any errors during test discovery, exit now.
   if (litConfig->getNumErrors() > 0) {
      std::cerr << litConfig->getNumErrors() << " errors, exiting." << std::endl;
      exit(2);
   }
   return tests;
}

std::list<std::shared_ptr<LitTestCase>> load_test_suite(const std::list<std::string> &inputs)
{
   LitConfigPointer litConfig = std::make_shared<LitConfig>(
            "lit",
            std::list<std::string>{},
            false,
            false,
            false,
            std::list<std::string>{},
            false,
            false,
            false,
         #ifdef POLAR_OS_WIN32
            true,
         #else
            false,
         #endif
            std::map<std::string, std::any>{});
   std::list<std::tuple<TestSuitePointer, TestList>> searchResults = find_tests_for_inputs(litConfig, inputs);
   TestList tests;
   for (auto &item : searchResults) {
      TestList &subtests = std::get<1>(item);
      for (auto &test : subtests) {
         tests.push_back(test);
      }
   }
   RunPointer run = std::make_shared<Run>(litConfig, tests);
   std::list<std::shared_ptr<LitTestCase>> testcases;
   for (auto &test : run->getTests()) {
      testcases.push_back(std::make_shared<LitTestCase>(test, run));
   }
   return testcases;
}

} // lit
} // polar
