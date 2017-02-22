//===-- BuildSystem.cpp ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildSystem.h"
#include "llbuild/BuildSystem/BuildSystemCommandInterface.h"
#include "llbuild/BuildSystem/CommandResult.h"

#include "llbuild/Basic/FileInfo.h"
#include "llbuild/Basic/FileSystem.h"
#include "llbuild/Basic/Hashing.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/Basic/PlatformUtility.h"
#include "llbuild/Basic/ShellUtility.h"
#include "llbuild/Core/BuildDB.h"
#include "llbuild/Core/BuildEngine.h"
#include "llbuild/Core/DependencyInfoParser.h"
#include "llbuild/Core/MakefileDepsParser.h"
#include "llbuild/BuildSystem/BuildExecutionQueue.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/BuildSystem/BuildNode.h"
#include "llbuild/BuildSystem/BuildValue.h"
#include "llbuild/BuildSystem/ExternalCommand.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

#include <unistd.h>

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::core;
using namespace llbuild::buildsystem;

BuildSystemDelegate::~BuildSystemDelegate() {}

BuildSystemCommandInterface::~BuildSystemCommandInterface() {}

#pragma mark - BuildSystem implementation

namespace {

class BuildSystemImpl;

/// The delegate used to load the build file for use by a build system.
class BuildSystemFileDelegate : public BuildFileDelegate {
  BuildSystemImpl& system;

public:
  BuildSystemFileDelegate(BuildSystemImpl& system)
      : BuildFileDelegate(), system(system) {}

  BuildSystemDelegate& getSystemDelegate();

  /// @name Delegate Implementation
  /// @{

  virtual FileSystem& getFileSystem() override {
    return getSystemDelegate().getFileSystem();
  }
  
  virtual void setFileContentsBeingParsed(StringRef buffer) override;
  
  virtual void error(StringRef filename,
                     const BuildFileToken& at,
                     const Twine& message) override;

  virtual bool configureClient(const ConfigureContext&, StringRef name,
                               uint32_t version,
                               const property_list_type& properties) override;

  virtual std::unique_ptr<Tool> lookupTool(StringRef name) override;

  virtual void loadedTarget(StringRef name,
                            const Target& target) override;

  virtual void loadedDefaultTarget(StringRef target) override;

  virtual void loadedCommand(StringRef name,
                             const Command& target) override;

  virtual std::unique_ptr<Node> lookupNode(StringRef name,
                                           bool isImplicit=false) override;

  /// @}
};

/// The delegate used to build a loaded build file.
class BuildSystemEngineDelegate : public BuildEngineDelegate {
  BuildSystemImpl& system;
  
  // FIXME: This is an inefficent map, the string is duplicated.
  std::unordered_map<std::string, std::unique_ptr<BuildNode>> dynamicNodes;

  /// The custom tasks which are owned by the build system.
  std::vector<std::unique_ptr<Command>> customTasks;

  const BuildDescription& getBuildDescription() const;

  virtual Rule lookupRule(const KeyType& keyData) override;
  virtual void cycleDetected(const std::vector<Rule*>& items) override;
  virtual void error(const Twine& message) override;

public:
  BuildSystemEngineDelegate(BuildSystemImpl& system) : system(system) {}

  BuildSystemImpl& getBuildSystem() {
    return system;
  }
};

class BuildSystemImpl : public BuildSystemCommandInterface {
  /// The internal schema version.
  ///
  /// Version History:
  /// * 6: Added DirectoryContents to BuildKey
  /// * 5: Switch BuildValue to be BinaryCoding based
  /// * 4: Pre-history
  static const uint32_t internalSchemaVersion = 6;
  
  BuildSystem& buildSystem;

  /// The delegate the BuildSystem was configured with.
  BuildSystemDelegate& delegate;

  /// The name of the main input file.
  std::string mainFilename;

  /// The delegate used for the loading the build file.
  BuildSystemFileDelegate fileDelegate;

  /// The build description, once loaded.
  std::unique_ptr<BuildDescription> buildDescription;

  /// The delegate used for building the file contents.
  BuildSystemEngineDelegate engineDelegate;

  /// The build engine.
  BuildEngine buildEngine;

  /// The execution queue reference; this is only valid while a build is
  /// actually in progress.
  std::unique_ptr<BuildExecutionQueue> executionQueue;

  /// Flag indicating if the build has been aborted.
  bool buildWasAborted = false;

  /// @name BuildSystemCommandInterface Implementation
  /// @{

  virtual BuildEngine& getBuildEngine() override {
    return buildEngine;
  }
  
  virtual BuildExecutionQueue& getExecutionQueue() override {
    assert(executionQueue.get());
    return *executionQueue;
  }

  virtual void taskNeedsInput(core::Task* task, const BuildKey& key,
                              uintptr_t inputID) override {
    return buildEngine.taskNeedsInput(task, key.toData(), inputID);
  }

  virtual void taskMustFollow(core::Task* task, const BuildKey& key) override {
    return buildEngine.taskMustFollow(task, key.toData());
  }

  virtual void taskDiscoveredDependency(core::Task* task,
                                        const BuildKey& key) override {
    return buildEngine.taskDiscoveredDependency(task, key.toData());
  }

  virtual void taskIsComplete(core::Task* task, const BuildValue& value,
                              bool forceChange) override {
    return buildEngine.taskIsComplete(task, value.toData(), forceChange);
  }

  virtual void addJob(QueueJob&& job) override {
    executionQueue->addJob(std::move(job));
  }

  /// @}

public:
  BuildSystemImpl(class BuildSystem& buildSystem,
                  BuildSystemDelegate& delegate)
      : buildSystem(buildSystem), delegate(delegate),
        fileDelegate(*this), engineDelegate(*this), buildEngine(engineDelegate),
        executionQueue() {}

  BuildSystem& getBuildSystem() {
    return buildSystem;
  }

  BuildSystemDelegate& getDelegate() override {
    return delegate;
  }

  // FIXME: We should eliminate this, it isn't well formed when loading
  // descriptions not from a file. We currently only use that for unit testing,
  // though.
  StringRef getMainFilename() {
    return mainFilename;
  }

  BuildSystemCommandInterface& getCommandInterface() {
    return *this;
  }

  const BuildDescription& getBuildDescription() const {
    assert(buildDescription);
    return *buildDescription;
  }

  void error(StringRef filename, const Twine& message) {
    getDelegate().error(filename, {}, message);
  }

  void error(StringRef filename, const BuildSystemDelegate::Token& at,
             const Twine& message) {
    getDelegate().error(filename, at, message);
  }

  std::unique_ptr<BuildNode> lookupNode(StringRef name,
                                        bool isImplicit);

  uint32_t getMergedSchemaVersion() {
    // FIXME: Find a cleaner strategy for merging the internal schema version
    // with that from the client.
    auto clientVersion = delegate.getVersion();
    assert(clientVersion <= (1 << 16) && "unsupported client verison");
    return internalSchemaVersion + (clientVersion << 16);
  }
  
  /// @name Client API
  /// @{

  bool loadDescription(StringRef filename) {
    this->mainFilename = filename;

    auto description = BuildFile(filename, fileDelegate).load();
    if (!description) {
      error(getMainFilename(), "unable to load build file");
      return false;
    }

    buildDescription = std::move(description);
    return true;
  }

  void loadDescription(std::unique_ptr<BuildDescription> description) {
    buildDescription = std::move(description);
  }

  bool attachDB(StringRef filename, std::string* error_out) {
    // FIXME: How do we pass the client schema version here, if we haven't
    // loaded the file yet.
    std::unique_ptr<core::BuildDB> db(
        core::createSQLiteBuildDB(filename, getMergedSchemaVersion(),
                                  error_out));
    if (!db)
      return false;

    return buildEngine.attachDB(std::move(db), error_out);
  }

  bool enableTracing(StringRef filename, std::string* error_out) {
    return buildEngine.enableTracing(filename, error_out);
  }

  /// Build the given key, and return the result and an indication of success.
  llvm::Optional<BuildValue> build(BuildKey key);
  
  bool build(StringRef target);

  void setBuildWasAborted(bool value) {
    buildWasAborted = value;
  }
    
  /// @}
};

#pragma mark - BuildSystem engine integration

#pragma mark - Task implementations

static BuildSystemImpl& getBuildSystem(BuildEngine& engine) {
  return static_cast<BuildSystemEngineDelegate*>(
      engine.getDelegate())->getBuildSystem();
}

static bool isCancelled(BuildEngine& engine) {
  return getBuildSystem(engine).getCommandInterface().getDelegate().isCancelled();
}
  
/// This is the task used to "build" a target, it translates between the request
/// for building a target key and the requests for all of its nodes.
class TargetTask : public Task {
  Target& target;
  
  // Build specific data.
  //
  // FIXME: We should probably factor this out somewhere else, so we can enforce
  // it is never used when initialized incorrectly.

  /// If true, the command had a missing input (this implies ShouldSkip is
  /// true).
  bool hasMissingInput = false;

  virtual void start(BuildEngine& engine) override {
    // Request all of the necessary system tasks.
    unsigned id = 0;
    for (auto it = target.getNodes().begin(),
           ie = target.getNodes().end(); it != ie; ++it, ++id) {
      engine.taskNeedsInput(this, BuildKey::makeNode(*it).toData(), id);
    }
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
    // Do nothing.
  }

  virtual void provideValue(BuildEngine& engine, uintptr_t inputID,
                            const ValueType& valueData) override {
    // Do nothing.
    auto value = BuildValue::fromData(valueData);

    if (value.isMissingInput()) {
      hasMissingInput = true;

      // FIXME: Design the logging and status output APIs.
      auto& system = getBuildSystem(engine);
      system.error(system.getMainFilename(),
                   (Twine("missing input '") +
                    target.getNodes()[inputID]->getName() +
                    "' and no rule to build it"));
    }
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // If the build should cancel, do nothing.
    if (isCancelled(engine)) {
      engine.taskIsComplete(this, BuildValue::makeSkippedCommand().toData());
      return;
    }

    if (hasMissingInput) {
      // FIXME: Design the logging and status output APIs.
      auto& system = getBuildSystem(engine);
      system.error(system.getMainFilename(),
                   (Twine("cannot build target '") + target.getName() +
                    "' due to missing input"));
      
      // Report the command failure.
      system.getDelegate().hadCommandFailure();
    }
    
    // Complete the task immediately.
    engine.taskIsComplete(this, BuildValue::makeTarget().toData());
  }

public:
  TargetTask(Target& target) : target(target) {}

  static bool isResultValid(BuildEngine& engine, Target& node,
                            const BuildValue& value) {
    // Always treat target tasks as invalid.
    return false;
  }
};


/// This is the task to "build" a file node which represents pure raw input to
/// the system.
class FileInputNodeTask : public Task {
  BuildNode& node;

  virtual void start(BuildEngine& engine) override {
    assert(node.getProducers().empty());
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
  }

  virtual void inputsAvailable(BuildEngine& engine) override {    
    // FIXME: We should do this work in the background.

    // Get the information on the file.
    //
    // FIXME: This needs to delegate, since we want to have a notion of
    // different node types.
    assert(!node.isVirtual());
    auto info = node.getFileInfo(
        getBuildSystem(engine).getDelegate().getFileSystem());
    if (info.isMissing()) {
      engine.taskIsComplete(this, BuildValue::makeMissingInput().toData());
      return;
    }

    engine.taskIsComplete(
        this, BuildValue::makeExistingInput(info).toData());
  }

public:
  FileInputNodeTask(BuildNode& node) : node(node) {
    assert(!node.isVirtual());
  }

  static bool isResultValid(BuildEngine& engine, const BuildNode& node,
                            const BuildValue& value) {
    // The result is valid if the existence matches the value type and the file
    // information remains the same.
    //
    // FIXME: This is inefficient, we will end up doing the stat twice, once
    // when we check the value for up to dateness, and once when we "build" the
    // output.
    //
    // We can solve this by caching ourselves but I wonder if it is something
    // the engine should support more naturally.
    auto info = node.getFileInfo(
        getBuildSystem(engine).getDelegate().getFileSystem());
    if (info.isMissing()) {
      return value.isMissingInput();
    } else {
      return value.isExistingInput() && value.getOutputInfo() == info;
    }
  }
};


/// This is the task to "build" a directory node.
///
/// This node effectively just adapts a directory tree signature to a node. The
/// reason why we need it (versus simply making the directory tree signature
/// *be* this, is that we want the directory signature to be able to interface
/// with other build nodes produced by commands).
class DirectoryInputNodeTask : public Task {
  BuildNode& node;

  core::ValueType directorySignature;

  virtual void start(BuildEngine& engine) override {
    // Remove any trailing slash from the node name.
    StringRef path =  node.getName();
    if (path.endswith("/") && path != "/") {
      path = path.substr(0, path.size() - 1);
    }
    engine.taskNeedsInput(
        this, BuildKey::makeDirectoryTreeSignature(path).toData(),
        /*inputID=*/0);
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
    directorySignature = value;
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // Simply propagate the value.
    engine.taskIsComplete(this, ValueType(directorySignature));
  }

public:
  DirectoryInputNodeTask(BuildNode& node) : node(node) {
    assert(!node.isVirtual());
  }
};


/// This is the task to build a virtual node which isn't connected to any
/// output.
class VirtualInputNodeTask : public Task {
  virtual void start(BuildEngine& engine) override {
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    engine.taskIsComplete(
        this, BuildValue::makeVirtualInput().toData());
  }

public:
  VirtualInputNodeTask() {}

  static bool isResultValid(BuildEngine& engine, const BuildNode& node,
                            const BuildValue& value) {
    // Virtual input nodes are always valid unless the value type is wrong.
    return value.isVirtualInput();
  }
};


/// This is the task to "build" a node which is the product of some command.
///
/// It is responsible for selecting the appropriate producer command to run to
/// produce the node, and for synchronizing any external state the node depends
/// on.
class ProducedNodeTask : public Task {
  Node& node;
  BuildValue nodeResult;
  Command* producingCommand = nullptr;

  // Build specific data.
  //
  // FIXME: We should probably factor this out somewhere else, so we can enforce
  // it is never used when initialized incorrectly.

  // Whether this is a node we are unable to produce.
  bool isInvalid = false;
  
  virtual void start(BuildEngine& engine) override {
    // Request the producer command.
    if (node.getProducers().size() == 1) {
      producingCommand = node.getProducers()[0];
      engine.taskNeedsInput(this, BuildKey::makeCommand(
                                producingCommand->getName()).toData(),
                            /*InputID=*/0);
      return;
    }

    // We currently do not support building nodes which have multiple producers.
    auto producerA = node.getProducers()[0];
    auto producerB = node.getProducers()[1];
    getBuildSystem(engine).error(
        "", "unable to build node: '" + node.getName() + "' (node is produced "
        "by multiple commands; e.g., '" + producerA->getName() + "' and '" +
        producerB->getName() + "')");
    isInvalid = true;
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& valueData) override {
    auto value = BuildValue::fromData(valueData);

    // Extract the node result from the command.
    assert(producingCommand);
    nodeResult = producingCommand->getResultForOutput(&node, value);
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    if (isInvalid) {
      engine.taskIsComplete(this, BuildValue::makeFailedInput().toData());
      return;
    }
    
    assert(!nodeResult.isInvalid());
    
    // Complete the task immediately.
    engine.taskIsComplete(this, nodeResult.toData());
  }

public:
  ProducedNodeTask(Node& node)
      : node(node), nodeResult(BuildValue::makeInvalid()) {}
  
  static bool isResultValid(BuildEngine&, Node& node,
                            const BuildValue& value) {
    // If the result was failure, we always need to rebuild (it may produce an
    // error).
    if (value.isFailedInput())
      return false;

    // The produced node result itself doesn't need any synchronization.
    return true;
  }
};


/// This task is responsible for computing the lists of files in directories.
class DirectoryContentsTask : public Task {
  std::string path;

  virtual void start(BuildEngine& engine) override {
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // FIXME: We should do this work in the background.
    
    // Get the stat information on the directory.
    auto info = getBuildSystem(engine).getDelegate().getFileSystem().getFileInfo(
        path);
    if (info.isMissing()) {
      engine.taskIsComplete(
          this, BuildValue::makeMissingInput().toData());
      return;
    }

    // Get the list of files in the directory.
    std::error_code ec;
    std::vector<std::string> filenames;
    for (auto it = llvm::sys::fs::directory_iterator(path, ec),
           end = llvm::sys::fs::directory_iterator(); it != end;
         it = it.increment(ec)) {
      filenames.push_back(llvm::sys::path::filename(it->path()));
    }

    // Order the filenames.
    std::sort(filenames.begin(), filenames.end(),
              [](const std::string& a, const std::string& b) {
                return a < b;
              });

    // Create the result.
    engine.taskIsComplete(
        this, BuildValue::makeDirectoryContents(info, filenames).toData());
  }

public:
  DirectoryContentsTask(StringRef path) : path(path) {}

  static bool isResultValid(BuildEngine& engine, StringRef path,
                            const BuildValue& value) {
    // The result is valid if the existence matches the existing value type, and
    // the file information remains the same.
    auto info = getBuildSystem(engine).getDelegate().getFileSystem().getFileInfo(
        path);
    if (info.isMissing()) {
      return value.isMissingInput();
    } else {
      return value.isExistingInput() && value.getOutputInfo() == info;
    }
  }
};



/// This is the task to "build" a directory node which will encapsulate (via a
/// signature) all of the contents of the directory, recursively.
class DirectoryTreeSignatureTask : public Task {
  // The basic algorithm we need to follow:
  //
  // 1. Get the directory contents.
  // 2. Get the subpath directory info.
  // 3. For each node input, if it is a directory, get the input node for it.
  //
  // FIXME: This algorithm currently does a redundant stat for each directory,
  // because we stat it once to find out it is a directory, then again when we
  // gather its contents (to use for validating the directory contents).
  //
  // FIXME: We need to fix the directory list to not get contents for symbolic
  // links.

  /// This structure encapsulates the information we need on each child.
  struct SubpathInfo {
    /// The filename;
    std::string filename;
    
    /// The result of requesting the node at this subpath, once available.
    ValueType value;

    /// The directory signature, if needed.
    llvm::Optional<ValueType> directorySignatureValue;
  };
  
  /// The path we are taking the signature of.
  std::string path;

  /// The value for the directory itself.
  ValueType directoryValue;

  /// The accumulated list of child input info.
  ///
  /// Once we have the input directory information, we resize this to match the
  /// number of children to avoid dynamically resizing it.
  std::vector<SubpathInfo> childResults;
  
  virtual void start(BuildEngine& engine) override {
    // Ask for the base directory directory contents.
    engine.taskNeedsInput(
        this, BuildKey::makeDirectoryContents(path).toData(),
        /*inputID=*/0);
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine& engine, uintptr_t inputID,
                            const ValueType& valueData) override {
    // The first input is the directory contents.
    if (inputID == 0) {
      // Record the value for the directory.
      directoryValue = valueData;

      // Request the inputs for each subpath.
      auto value = BuildValue::fromData(valueData);
      if (value.isMissingInput())
        return;

      assert(value.isDirectoryContents());
      auto filenames = value.getDirectoryContents();
      for (size_t i = 0; i != filenames.size(); ++i) {
        SmallString<256> childPath{ path };
        llvm::sys::path::append(childPath, filenames[i]);
        childResults.emplace_back(SubpathInfo{ filenames[i], {}, None });
        engine.taskNeedsInput(this, BuildKey::makeNode(childPath).toData(),
                              /*inputID=*/1 + i);
      }
      return;
    }

    // If the input is a child, add it to the collection and dispatch a
    // directory request if needed.
    if (inputID >= 1 && inputID < 1 + childResults.size()) {
      auto index = inputID - 1;
      auto& childResult = childResults[index];
      childResult.value = valueData;

      // If this node is a directory, request its signature recursively.
      auto value = BuildValue::fromData(valueData);
      if (value.isExistingInput()) {
        if (value.getOutputInfo().isDirectory()) {
          SmallString<256> childPath{ path };
          llvm::sys::path::append(childPath, childResult.filename);
        
          engine.taskNeedsInput(
              this, BuildKey::makeDirectoryTreeSignature(childPath).toData(),
              /*inputID=*/1 + childResults.size() + index);
        }
      }
      return;
    }

    // Otherwise, the input should be a directory signature.
    auto index = inputID - 1 - childResults.size();
    assert(index < childResults.size());
    childResults[index].directorySignatureValue = valueData;
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // Compute the signature across all of the inputs.
    using llvm::hash_combine;
    llvm::hash_code code = hash_value(path);

    // Add the signature for the actual input path.
    code = hash_combine(
        code, hash_combine_range(directoryValue.begin(), directoryValue.end()));

    // For now, we represent this task as the aggregation of all the inputs.
    for (const auto& info: childResults) {
      // We merge the children by simply combining their encoded representation.
      code = hash_combine(
          code, hash_combine_range(info.value.begin(), info.value.end()));
      if (info.directorySignatureValue.hasValue()) {
        auto& data = info.directorySignatureValue.getValue();
        code = hash_combine(
            code, hash_combine_range(data.begin(), data.end()));
      } else {
        // Combine a random number to represent nil.
        code = hash_combine(code, 0XC183979C3E98722E);
      }
    }
    
    // Compute the signature.
    engine.taskIsComplete(this, BuildValue::makeDirectoryTreeSignature(
                              uint64_t(code)).toData());
  }

public:
  DirectoryTreeSignatureTask(StringRef path) : path(path) {}
};


/// This is the task to actually execute a command.
class CommandTask : public Task {
  Command& command;

  virtual void start(BuildEngine& engine) override {
    command.start(getBuildSystem(engine).getCommandInterface(), this);
  }

  virtual void providePriorValue(BuildEngine& engine,
                                 const ValueType& valueData) override {
    BuildValue value = BuildValue::fromData(valueData);
    command.providePriorValue(
        getBuildSystem(engine).getCommandInterface(), this, value);
  }

  virtual void provideValue(BuildEngine& engine, uintptr_t inputID,
                            const ValueType& valueData) override {
    command.provideValue(
        getBuildSystem(engine).getCommandInterface(), this, inputID,
        BuildValue::fromData(valueData));
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // If the build should cancel, do nothing.
    if (isCancelled(engine)) {
      engine.taskIsComplete(this, BuildValue::makeSkippedCommand().toData());
      return;
    }

    auto& bsci = getBuildSystem(engine).getCommandInterface();
    auto fn = [this, &bsci=bsci](QueueJobContext* context) {
      bool shouldSkip = !bsci.getDelegate().shouldCommandStart(&command);

      if (shouldSkip) {
        // We need to call commandFinished here because commandPreparing and
        // shouldCommandStart guarantee that they're followed by
        // commandFinished.
        bsci.getDelegate().commandFinished(&command);

        bsci.taskIsComplete(this, BuildValue::makeSkippedCommand());
      } else {
        command.inputsAvailable(bsci, this);
      }
    };

    bsci.addJob({ &command, std::move(fn) });
  }

public:
  CommandTask(Command& command) : command(command) {}

  static bool isResultValid(BuildEngine& engine, Command& command,
                            const BuildValue& value) {
    // Delegate to the command for further checking.
    return command.isResultValid(
        getBuildSystem(engine).getBuildSystem(), value);
  }
};

#pragma mark - BuildSystemEngineDelegate implementation

/// This is a synthesized task used to represent a missing command.
///
/// This command is used in cases where a command has been removed from the
/// manifest, but can still be found during an incremental rebuild. This command
/// is used to inject an invalid value thus forcing downstream clients to
/// rebuild.
class MissingCommandTask : public Task {
private:
  virtual void start(BuildEngine& engine) override { }
  virtual void providePriorValue(BuildEngine& engine,
                                 const ValueType& valueData) override { }

  virtual void provideValue(BuildEngine& engine, uintptr_t inputID,
                            const ValueType& valueData) override { }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // A missing command always builds to an invalid value, and forces
    // downstream clients to be rebuilt (at which point they will presumably see
    // the command is no longer used).
    return engine.taskIsComplete(this, BuildValue::makeInvalid().toData(),
                                 /*forceChange=*/true);
  }

public:
  using Task::Task;
};

const BuildDescription& BuildSystemEngineDelegate::getBuildDescription() const {
  return system.getBuildDescription();
}

static BuildSystemDelegate::CommandStatusKind
convertStatusKind(core::Rule::StatusKind kind) {
  switch (kind) {
  case core::Rule::StatusKind::IsScanning:
    return BuildSystemDelegate::CommandStatusKind::IsScanning;
  case core::Rule::StatusKind::IsUpToDate:
    return BuildSystemDelegate::CommandStatusKind::IsUpToDate;
  case core::Rule::StatusKind::IsComplete:
    return BuildSystemDelegate::CommandStatusKind::IsComplete;
  }
  assert(0 && "unknown status kind");
  return BuildSystemDelegate::CommandStatusKind::IsScanning;
}

Rule BuildSystemEngineDelegate::lookupRule(const KeyType& keyData) {
  // Decode the key.
  auto key = BuildKey::fromData(keyData);

  switch (key.getKind()) {
  case BuildKey::Kind::Unknown:
    break;
    
  case BuildKey::Kind::Command: {
    // Find the comand.
    auto it = getBuildDescription().getCommands().find(key.getCommandName());
    if (it == getBuildDescription().getCommands().end()) {
      // If there is no such command, produce an error task.
      return Rule{
        keyData,
        /*Action=*/ [](BuildEngine& engine) -> Task* {
          return engine.registerTask(new MissingCommandTask());
        },
        /*IsValid=*/ [](BuildEngine&, const Rule& rule,
                        const ValueType& value) -> bool {
          // The cached result for a missing command is never valid.
          return false;
        }
      };
    }

    // Create the rule for the command.
    Command* command = it->second.get();
    return Rule{
      keyData,
      /*Action=*/ [command](BuildEngine& engine) -> Task* {
        return engine.registerTask(new CommandTask(*command));
      },
      /*IsValid=*/ [command](BuildEngine& engine, const Rule& rule,
                             const ValueType& value) -> bool {
        return CommandTask::isResultValid(
            engine, *command, BuildValue::fromData(value));
      },
      /*UpdateStatus=*/ [command](BuildEngine& engine,
                                  core::Rule::StatusKind status) {
        return ::getBuildSystem(engine).getDelegate().commandStatusChanged(
            command, convertStatusKind(status));
      }
    };
  }

  case BuildKey::Kind::CustomTask: {
    // Search for a tool which knows how to create the given custom task.
    //
    // FIXME: We should most likely have some kind of registration process so we
    // can do an efficient query here, but exactly how this should look isn't
    // clear yet.
    for (const auto& it: getBuildDescription().getTools()) {
      auto result = it.second->createCustomCommand(key);
      if (!result) continue;

      // Save the custom command.
      customTasks.emplace_back(std::move(result));
      Command *command = customTasks.back().get();
      
      return Rule{
        keyData,
        /*Action=*/ [command](BuildEngine& engine) -> Task* {
          return engine.registerTask(new CommandTask(*command));
        },
        /*IsValid=*/ [command](BuildEngine& engine, const Rule& rule,
                               const ValueType& value) -> bool {
          return CommandTask::isResultValid(
              engine, *command, BuildValue::fromData(value));
        }
      };
    }
    
    // We were unable to create an appropriate custom command, produce an error
    // task.
    return Rule{
      keyData,
      /*Action=*/ [](BuildEngine& engine) -> Task* {
        return engine.registerTask(new MissingCommandTask());
      },
      /*IsValid=*/ [](BuildEngine&, const Rule& rule,
                      const ValueType& value) -> bool {
        // The cached result for a missing command is never valid.
        return false;
      }
    };
  }

  case BuildKey::Kind::DirectoryContents: {
    std::string path = key.getDirectoryContentsPath();
    return Rule{
      keyData,
      /*Action=*/ [path](BuildEngine& engine) -> Task* {
        return engine.registerTask(new DirectoryContentsTask(path));
      },
      /*IsValid=*/ [path](BuildEngine& engine, const Rule& rule,
                          const ValueType& value) -> bool {
        return DirectoryContentsTask::isResultValid(
            engine, path, BuildValue::fromData(value));
      }
    };
  }

  case BuildKey::Kind::DirectoryTreeSignature: {
    std::string path = key.getDirectoryTreeSignaturePath();
    return Rule{
      keyData,
      /*Action=*/ [path](BuildEngine& engine) -> Task* {
        return engine.registerTask(new DirectoryTreeSignatureTask(path));
      },
        // Directory signatures don't require any validation outside of their
        // concrete dependencies.
      /*IsValid=*/ nullptr
    };
  }
    
  case BuildKey::Kind::Node: {
    // Find the node.
    auto it = getBuildDescription().getNodes().find(key.getNodeName());
    BuildNode* node;
    if (it != getBuildDescription().getNodes().end()) {
      node = static_cast<BuildNode*>(it->second.get());
    } else {
      auto it = dynamicNodes.find(key.getNodeName());
      if (it != dynamicNodes.end()) {
        node = it->second.get();
      } else {
        // Create nodes on the fly for any unknown ones.
        auto nodeOwner = system.lookupNode(
            key.getNodeName(), /*isImplicit=*/true);
        node = nodeOwner.get();
        dynamicNodes[key.getNodeName()] = std::move(nodeOwner);
      }
    }

    // Create the rule used to construct this node.
    //
    // We could bypass this level and directly return the rule to run the
    // command, which would reduce the number of tasks in the system. For now we
    // do the uniform thing, but do differentiate between input and command
    // nodes.

    // Create an input node if there are no producers.
    if (node->getProducers().empty()) {
      if (node->isVirtual()) {
        return Rule{
          keyData,
          /*Action=*/ [node](BuildEngine& engine) -> Task* {
            return engine.registerTask(new VirtualInputNodeTask());
          },
          /*IsValid=*/ [node](BuildEngine& engine, const Rule& rule,
                                const ValueType& value) -> bool {
            return VirtualInputNodeTask::isResultValid(
                engine, *node, BuildValue::fromData(value));
          }
        };
      }

      if (node->isDirectory()) {
        return Rule{
          keyData,
            /*Action=*/ [node](BuildEngine& engine) -> Task* {
            return engine.registerTask(new DirectoryInputNodeTask(*node));
          },
            // Directory nodes don't require any validation outside of their
            // concrete dependencies.
          /*IsValid=*/ nullptr
        };
      }
      
      return Rule{
        keyData,
        /*Action=*/ [node](BuildEngine& engine) -> Task* {
          return engine.registerTask(new FileInputNodeTask(*node));
        },
        /*IsValid=*/ [node](BuildEngine& engine, const Rule& rule,
                            const ValueType& value) -> bool {
          return FileInputNodeTask::isResultValid(
              engine, *node, BuildValue::fromData(value));
        }
      };
    }

    // Otherwise, create a task for a produced node.
    return Rule{
      keyData,
      /*Action=*/ [node](BuildEngine& engine) -> Task* {
        return engine.registerTask(new ProducedNodeTask(*node));
      },
      /*IsValid=*/ [node](BuildEngine& engine, const Rule& rule,
                          const ValueType& value) -> bool {
        return ProducedNodeTask::isResultValid(
            engine, *node, BuildValue::fromData(value));
      }
    };
  }

  case BuildKey::Kind::Target: {
    // Find the target.
    auto it = getBuildDescription().getTargets().find(key.getTargetName());
    if (it == getBuildDescription().getTargets().end()) {
      // FIXME: Invalid target name, produce an error.
      assert(0 && "FIXME: invalid target");
      abort();
    }

    // Create the rule to construct this target.
    Target* target = it->second.get();
    return Rule{
      keyData,
        /*Action=*/ [target](BuildEngine& engine) -> Task* {
        return engine.registerTask(new TargetTask(*target));
      },
      /*IsValid=*/ [target](BuildEngine& engine, const Rule& rule,
                            const ValueType& value) -> bool {
        return TargetTask::isResultValid(
            engine, *target, BuildValue::fromData(value));
      }
    };
  }
  }

  assert(0 && "invalid key type");
  abort();
}

void BuildSystemEngineDelegate::cycleDetected(const std::vector<Rule*>& cycle) {
  // Track that the build has been aborted.
  getBuildSystem().setBuildWasAborted(true);
  
  // Compute a description of the cycle path.
  SmallString<256> message;
  llvm::raw_svector_ostream os(message);
  os << "cycle detected while building: ";
  bool first = true;
  for (const auto* rule: cycle) {
    if (!first)
      os << " -> ";

    // Convert to a build key.
    auto key = BuildKey::fromData(rule->key);
    switch (key.getKind()) {
    case BuildKey::Kind::Unknown:
      os << "((unknown))";
      break;
    case BuildKey::Kind::Command:
      os << "command '" << key.getCommandName() << "'";
      break;
    case BuildKey::Kind::CustomTask:
      os << "custom task '" << key.getCustomTaskName() << "'";
      break;
    case BuildKey::Kind::DirectoryContents:
      os << "directory-contents '" << key.getDirectoryContentsPath() << "'";
      break;
    case BuildKey::Kind::DirectoryTreeSignature:
      os << "directory-tree-signature '"
         << key.getDirectoryTreeSignaturePath() << "'";
      break;
    case BuildKey::Kind::Node:
      os << "node '" << key.getNodeName() << "'";
      break;
    case BuildKey::Kind::Target:
      os << "target '" << key.getTargetName() << "'";
      break;
    }
    first = false;
  }
  
  system.error(system.getMainFilename(), os.str());
}

void BuildSystemEngineDelegate::error(const Twine& message) {
  system.error(system.getMainFilename(), message);
}

#pragma mark - BuildSystemImpl implementation

std::unique_ptr<BuildNode>
BuildSystemImpl::lookupNode(StringRef name, bool isImplicit) {
  bool isDirectory = name.endswith("/");
  bool isVirtual = !name.empty() && name[0] == '<' && name.back() == '>';
  return llvm::make_unique<BuildNode>(name, isDirectory, isVirtual,
                                      /*isCommandTimestamp=*/false,
                                      /*isMutable=*/false);
}

llvm::Optional<BuildValue> BuildSystemImpl::build(BuildKey key) {
  // Create the execution queue.
  executionQueue = delegate.createExecutionQueue();

  // Build the target.
  buildWasAborted = false;
  auto result = getBuildEngine().build(key.toData());
    
  // Release the execution queue, impicitly waiting for it to complete. The
  // asynchronous nature of the engine callbacks means it is possible for the
  // queue to have notified the engine of the last task completion, but still
  // have other work to perform (e.g., informing the client of command
  // completion).
  executionQueue.reset();

  if (buildWasAborted)
    return None;
  return BuildValue::fromData(result);
}

bool BuildSystemImpl::build(StringRef target) {
  // The build description must have been loaded.
  if (!buildDescription) {
    error(getMainFilename(), "no build description loaded");
    return false;
  }

  // If target name is not passed then we try to load the default target name
  // from manifest file
  if (target.empty()) {
    target = getBuildDescription().getDefaultTarget();
  }

  return build(BuildKey::makeTarget(target)).hasValue();
}

#pragma mark - PhonyTool implementation

class PhonyCommand : public ExternalCommand {
public:
  using ExternalCommand::ExternalCommand;

  virtual bool shouldShowStatus() override { return false; }

  virtual void getShortDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream(result) << getName();
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream(result) << getName();
  }

  virtual CommandResult executeExternalCommand(BuildSystemCommandInterface& bsci,
                                               Task* task,
                                               QueueJobContext* context) override {
    // Nothing needs to be done for phony commands.
    return CommandResult::Succeeded;
  }
};

class PhonyTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported configuration attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported configuration attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<PhonyCommand>(name);
  }
};

#pragma mark - ShellTool implementation

class ShellCommand : public ExternalCommand {
  /// The dependencies style to expect (in the `depsPath`).
  enum class DepsStyle {
    /// No discovered dependencies are in use.
    Unused = 0,
      
    /// "Makefile" style dependencies in the form typically generated by C
    /// compilers, wherein the dependencies of the first target are treated as
    /// dependencies of the command.
    Makefile,

    /// Darwin's DependencyInfo format.
    DependencyInfo,
  };
  
  /// The command line arguments.
  std::vector<std::string> args;

  /// The environment to use. If empty, the environment will be inherited.
  llvm::StringMap<std::string> env;
  
  /// The path to the dependency output file, if used.
  SmallVector<std::string, 1> depsPaths{};

  /// The style of dependencies used.
  DepsStyle depsStyle = DepsStyle::Unused;

  /// Whether to inherit the base environment.
  bool inheritEnv = true;

  virtual uint64_t getSignature() override {
    // FIXME: Use a more appropriate hashing infrastructure.
    using llvm::hash_combine;
    llvm::hash_code code = ExternalCommand::getSignature();
    for (const auto& arg: args) {
      code = hash_combine(code, arg);
    }
    for (const auto& entry: env) {
      code = hash_combine(code, entry.getKey());
      code = hash_combine(code, entry.getValue());
    }
    for (const auto& path: depsPaths) {
      code = hash_combine(code, path);
    }
    code = hash_combine(code, int(depsStyle));
    code = hash_combine(code, int(inheritEnv));
    return size_t(code);
  }

  bool processDiscoveredDependencies(BuildSystemCommandInterface& bsci,
                                     Task* task,
                                     QueueJobContext* context) {
    // It is an error if the dependencies style is not specified.
    //
    // FIXME: Diagnose this sooner.
    if (depsStyle == DepsStyle::Unused) {
      getBuildSystem(bsci.getBuildEngine()).error(
          "", "missing required 'deps-style' specifier");
      return false;
    }

    for (const auto& depsPath: depsPaths) {
      // Read the dependencies file.
      auto input = bsci.getDelegate().getFileSystem().getFileContents(depsPath);
      if (!input) {
        getBuildSystem(bsci.getBuildEngine()).error(
            depsPath, "unable to open dependencies file (" + depsPath + ")");
        return false;
      }

      switch (depsStyle) {
      case DepsStyle::Unused:
        assert(0 && "unreachable");
        break;

      case DepsStyle::Makefile:
        if (!processMakefileDiscoveredDependencies(
                bsci, task, context, depsPath, input.get()))
          return false;
        continue;

      case DepsStyle::DependencyInfo:
        if (!processDependencyInfoDiscoveredDependencies(
                bsci, task, context, depsPath, input.get()))
          return false;
        continue;
      }
      
      llvm::report_fatal_error("unknown dependencies style");
    }

    return true;
  }

  bool processMakefileDiscoveredDependencies(BuildSystemCommandInterface& bsci,
                                             Task* task,
                                             QueueJobContext* context,
                                             StringRef depsPath,
                                             llvm::MemoryBuffer* input) {
    // Parse the output.
    //
    // We just ignore the rule, and add any dependency that we encounter in the
    // file.
    struct DepsActions : public core::MakefileDepsParser::ParseActions {
      BuildSystemCommandInterface& bsci;
      Task* task;
      ShellCommand* command;
      StringRef depsPath;
      unsigned numErrors{0};

      DepsActions(BuildSystemCommandInterface& bsci, Task* task,
                  ShellCommand* command, StringRef depsPath)
          : bsci(bsci), task(task), command(command), depsPath(depsPath) {}

      virtual void error(const char* message, uint64_t position) override {
        getBuildSystem(bsci.getBuildEngine()).error(
            depsPath, "error reading dependency file: " + std::string(message));
        ++numErrors;
      }

      virtual void actOnRuleDependency(const char* dependency,
                                       uint64_t length,
                                       const StringRef unescapedWord) override {
        bsci.taskDiscoveredDependency(task, BuildKey::makeNode(unescapedWord));
      }

      virtual void actOnRuleStart(const char* name, uint64_t length,
                                  const StringRef unescapedWord) override {}

      virtual void actOnRuleEnd() override {}
    };

    DepsActions actions(bsci, task, this, depsPath);
    core::MakefileDepsParser(input->getBufferStart(), input->getBufferSize(),
                             actions).parse();
    return actions.numErrors == 0;
  }

  bool
  processDependencyInfoDiscoveredDependencies(BuildSystemCommandInterface& bsci,
                                              Task* task,
                                              QueueJobContext* context,
                                              StringRef depsPath,
                                              llvm::MemoryBuffer* input) {
    // Parse the output.
    //
    // We just ignore the rule, and add any dependency that we encounter in the
    // file.
    struct DepsActions : public core::DependencyInfoParser::ParseActions {
      BuildSystemCommandInterface& bsci;
      Task* task;
      ShellCommand* command;
      StringRef depsPath;
      unsigned numErrors{0};

      DepsActions(BuildSystemCommandInterface& bsci, Task* task,
                  ShellCommand* command, StringRef depsPath)
          : bsci(bsci), task(task), command(command), depsPath(depsPath) {}

      virtual void error(const char* message, uint64_t position) override {
        getBuildSystem(bsci.getBuildEngine()).error(
            depsPath, "error reading dependency file: " + std::string(message));
        ++numErrors;
      }

      // Ignore everything but actual inputs.
      virtual void actOnVersion(StringRef) override { }
      virtual void actOnMissing(StringRef) override { }
      virtual void actOnOutput(StringRef) override { }

      virtual void actOnInput(StringRef name) override {
        bsci.taskDiscoveredDependency(task, BuildKey::makeNode(name));
      }
    };

    DepsActions actions(bsci, task, this, depsPath);
    core::DependencyInfoParser(input->getBuffer(), actions).parse();
    return actions.numErrors == 0;
  }

public:
  using ExternalCommand::ExternalCommand;

  virtual void getShortDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream(result) << getDescription();
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream os(result);
    bool first = true;
    for (const auto& arg: args) {
      if (!first) os << " ";
      first = false;
      basic::appendShellEscapedString(os, arg);
    }
  }
  
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "args") {
      // When provided as a scalar string, we default to executing using the
      // shell.
      args.clear();
      args.push_back("/bin/sh");
      args.push_back("-c");
      args.push_back(value);
    } else if (name == "deps") {
      depsPaths.clear();
      depsPaths.emplace_back(value);
    } else if (name == "deps-style") {
      if (value == "makefile") {
        depsStyle = DepsStyle::Makefile;
      } else if (value == "dependency-info") {
        depsStyle = DepsStyle::DependencyInfo;
      } else {
        ctx.error("unknown 'deps-style': '" + value + "'");
        return false;
      }
      return true;
    } else if (name == "inherit-env") {
      if (value != "true" && value != "false") {
        ctx.error("invalid value: '" + value + "' for attribute '" +
                  name + "'");
        return false;
      }
      inheritEnv = value == "true";
    } else {
      return ExternalCommand::configureAttribute(ctx, name, value);
    }

    return true;
  }
  
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    if (name == "args") {
      // Diagnose missing arguments.
      if (values.empty()) {
        ctx.error("invalid arguments for command '" + getName() + "'");
        return false;
      }
      
      args = std::vector<std::string>(values.begin(), values.end());
    } else if (name == "deps") {
      depsPaths.clear();
      depsPaths.insert(depsPaths.begin(), values.begin(), values.end());
    } else {
      return ExternalCommand::configureAttribute(ctx, name, values);
    }

    return true;
  }

  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    if (name == "env") {
      env.clear();
      for (const auto& entry: values) {
        env[entry.first] = entry.second;
      }
    } else {
      return ExternalCommand::configureAttribute(ctx, name, values);
    }

    return true;
  }

  virtual CommandResult executeExternalCommand(BuildSystemCommandInterface& bsci,
                                               Task* task,
                                               QueueJobContext* context) override {
    std::vector<std::pair<StringRef, StringRef>> environment;
    for (const auto& it: env) {
      environment.push_back({ it.getKey(), it.getValue() });
    }
    
    // Execute the command.
    auto result = bsci.getExecutionQueue().executeProcess(context, std::vector<StringRef>(args.begin(), args.end()),
                                                          environment, /*inheritEnvironment=*/inheritEnv);

    if (result != CommandResult::Succeeded) {
      // If the command failed, there is no need to gather dependencies.
      return result;
    }
    
    // Collect the discovered dependencies, if used.
    if (!depsPaths.empty()) {
      if (!processDiscoveredDependencies(bsci, task, context)) {
        // If we were unable to process the dependencies output, report a
        // failure.
        return CommandResult::Failed;
      }
    }
    
    return result;
  }
};

class ShellTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<ShellCommand>(name);
  }
};

#pragma mark - ClangTool implementation

class ClangShellCommand : public ExternalCommand {
  /// The compiler command to invoke.
  std::vector<std::string> args;
  
  /// The path to the dependency output file, if used.
  std::string depsPath;
  
  virtual uint64_t getSignature() override {
    using llvm::hash_combine;
    llvm::hash_code code = ExternalCommand::getSignature();
    for (const auto& arg: args) {
      code = hash_combine(code, arg);
    }
    return size_t(code);
  }

  bool processDiscoveredDependencies(BuildSystemCommandInterface& bsci,
                                     Task* task,
                                     QueueJobContext* context) {
    // Read the dependencies file.
    auto input = bsci.getDelegate().getFileSystem().getFileContents(depsPath);
    if (!input) {
      getBuildSystem(bsci.getBuildEngine()).error(
          depsPath, "unable to open dependencies file (" + depsPath + ")");
      return false;
    }

    // Parse the output.
    //
    // We just ignore the rule, and add any dependency that we encounter in the
    // file.
    struct DepsActions : public core::MakefileDepsParser::ParseActions {
      BuildSystemCommandInterface& bsci;
      Task* task;
      ClangShellCommand* command;
      unsigned numErrors{0};

      DepsActions(BuildSystemCommandInterface& bsci, Task* task,
                  ClangShellCommand* command)
          : bsci(bsci), task(task), command(command) {}

      virtual void error(const char* message, uint64_t position) override {
        getBuildSystem(bsci.getBuildEngine()).error(
            command->depsPath,
            "error reading dependency file: " + std::string(message));
        ++numErrors;
      }

      virtual void actOnRuleDependency(const char* dependency,
                                       uint64_t length,
                                       const StringRef unescapedWord) override {
        bsci.taskDiscoveredDependency(task, BuildKey::makeNode(unescapedWord));
      }

      virtual void actOnRuleStart(const char* name, uint64_t length,
                                  const StringRef unescapedWord) override {}

      virtual void actOnRuleEnd() override {}
    };

    DepsActions actions(bsci, task, this);
    core::MakefileDepsParser(input->getBufferStart(), input->getBufferSize(),
                             actions).parse();
    return actions.numErrors == 0;
  }

public:
  using ExternalCommand::ExternalCommand;

  virtual void getShortDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream(result) << getDescription();
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream os(result);
    bool first = true;
    for (const auto& arg: args) {
      if (!first) os << " ";
      first = false;
      basic::appendShellEscapedString(os, arg);
    }
  }
  
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "args") {
      // When provided as a scalar string, we default to executing using the
      // shell.
      args.clear();
      args.push_back("/bin/sh");
      args.push_back("-c");
      args.push_back(value);
    } else if (name == "deps") {
      depsPath = value;
    } else {
      return ExternalCommand::configureAttribute(ctx, name, value);
    }

    return true;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    if (name == "args") {
      args = std::vector<std::string>(values.begin(), values.end());
    } else {
        return ExternalCommand::configureAttribute(ctx, name, values);
    }

    return true;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    return ExternalCommand::configureAttribute(ctx, name, values);
  }

  virtual CommandResult executeExternalCommand(BuildSystemCommandInterface& bsci,
                                               Task* task,
                                               QueueJobContext* context) override {
    std::vector<StringRef> commandLine;
    for (const auto& arg: args) {
      commandLine.push_back(arg);
    }

    // Execute the command.
    auto result = bsci.getExecutionQueue().executeProcess(context, commandLine);

    if (result != CommandResult::Succeeded) {
      // If the command failed, there is no need to gather dependencies.
      return result;
    }

    // Otherwise, collect the discovered dependencies, if used.
    if (!depsPath.empty()) {
      if (!processDiscoveredDependencies(bsci, task, context)) {
        // If we were unable to process the dependencies output, report a
        // failure.
        return CommandResult::Failed;
      }
    }

    return result;
  }
};

class ClangTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<ClangShellCommand>(name);
  }
};

#pragma mark - MkdirTool implementation

class MkdirCommand : public ExternalCommand {
  virtual void getShortDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream(result) << getDescription();
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream os(result);
    os << "mkdir -p ";
    // FIXME: This isn't correct, we need utilities for doing shell quoting.
    if (StringRef(getOutputs()[0]->getName()).find(' ') != StringRef::npos) {
      os << '"' << getOutputs()[0]->getName() << '"';
    } else {
      os << getOutputs()[0]->getName();
    }
  }

  virtual bool isResultValid(BuildSystem& system,
                             const BuildValue& value) override {
    // If the prior value wasn't for a successful command, recompute.
    if (!value.isSuccessfulCommand())
      return false;

    // Otherwise, the result is valid if the directory still exists.
    auto info = getOutputs()[0]->getFileInfo(
        system.getDelegate().getFileSystem());
    if (info.isMissing())
      return false;

    // If the item is not a directory, it needs to be recreated.
    if (!info.isDirectory())
      return false;

    // FIXME: We should strictly enforce the integrity of this validity routine
    // by ensuring that the build result for this command does not fully encode
    // the file info, but rather just encodes its success. As is, we are leaking
    // out the details of the file info (like the timestamp), but not rerunning
    // when they change. This is by design for this command, but it would still
    // be nice to be strict about it.
    
    return true;
  }
  
  virtual CommandResult executeExternalCommand(BuildSystemCommandInterface& bsci,
                                               Task* task,
                                               QueueJobContext* context) override {
    auto output = getOutputs()[0];
    if (llvm::sys::fs::create_directories(output->getName())) {
      getBuildSystem(bsci.getBuildEngine()).error(
          "", "unable to create directory '" + output->getName() + "'");
      return CommandResult::Failed;
    }
    return CommandResult::Succeeded;
  }
  
public:
  using ExternalCommand::ExternalCommand;
};

class MkdirTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<MkdirCommand>(name);
  }
};

#pragma mark - SymlinkTool implementation

class SymlinkCommand : public Command {
  BuildNode* output = nullptr;

  /// The command description.
  std::string description;

  /// Declared command inputs, used only for ordering purposes.
  std::vector<BuildNode*> inputs;

  /// The contents to write at the output path.
  std::string contents;

  virtual uint64_t getSignature() {
    using llvm::hash_combine;
    llvm::hash_code code = hash_value(output->getName());
    code = hash_combine(code, contents);
    for (const auto* input: inputs) {
      code = hash_combine(code, input->getName());
    }
    return size_t(code);
  }

  virtual void configureDescription(const ConfigureContext&,
                                    StringRef value) override {
    description = value;
  }

  virtual void getShortDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream(result) << description;
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream os(result);
    os << "ln -sfh ";
    // FIXME: This isn't correct, we need utilities for doing shell quoting.
    if (StringRef(output->getName()).find(' ') != StringRef::npos) {
      os << '"' << output->getName() << '"';
    } else {
      os << output->getName();
    }
  }
  
  virtual void configureInputs(const ConfigureContext& ctx,
                               const std::vector<Node*>& value) override {
    inputs.reserve(value.size());
    for (auto* node: value) {
      inputs.emplace_back(static_cast<BuildNode*>(node));
    }
  }

  virtual void configureOutputs(const ConfigureContext& ctx,
                                const std::vector<Node*>& value) override {
    if (value.size() == 1) {
      output = static_cast<BuildNode*>(value[0]);
      if (output->isVirtual()) {
        ctx.error("unexpected virtual output");
      }
    } else if (value.empty()) {
      ctx.error("missing declared output");
    } else {
      ctx.error("unexpected explicit output: '" + value[1]->getName() + "'");
    }
  }
  
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (name == "contents") {
      contents = value;
      return true;
    } else {
      ctx.error("unexpected attribute: '" + name + "'");
      return false;
    }
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual BuildValue getResultForOutput(Node* node,
                                        const BuildValue& value) override {
    // If the value was a failed command, propagate the failure.
    if (value.isFailedCommand() || value.isPropagatedFailureCommand() ||
        value.isCancelledCommand())
      return BuildValue::makeFailedInput();
    if (value.isSkippedCommand())
      return BuildValue::makeSkippedCommand();

    // Otherwise, we should have a successful command -- return the actual
    // result for the output.
    assert(value.isSuccessfulCommand());

    return BuildValue::makeExistingInput(value.getOutputInfo());
  }

  virtual bool isResultValid(BuildSystem& system,
                             const BuildValue& value) override {
    // If the prior value wasn't for a successful command, recompute.
    if (!value.isSuccessfulCommand())
      return false;

    // Otherwise, assume the result is valid if its link status matches the
    // previous one.
    auto info = output->getLinkInfo(system.getDelegate().getFileSystem());
    if (info.isMissing())
      return false;

    return info == value.getOutputInfo();
  }
  
  virtual void start(BuildSystemCommandInterface& bsci,
                     core::Task* task) override {
    // Notify the client the command is preparing to run.
    bsci.getDelegate().commandPreparing(this);

    // The command itself takes no inputs, so just treat any declared inputs as
    // "must follow" directives.
    //
    // FIXME: We should make this explicit once we have actual support for must
    // follow inputs.
    for (auto it = inputs.begin(), ie = inputs.end(); it != ie; ++it) {
      bsci.taskMustFollow(task, BuildKey::makeNode(*it));
    }
  }

  virtual void providePriorValue(BuildSystemCommandInterface&, core::Task*,
                                 const BuildValue& value) override {
    // Ignored.
  }

  virtual void provideValue(BuildSystemCommandInterface&, core::Task*,
                            uintptr_t inputID,
                            const BuildValue& value) override {
    assert(0 && "unexpected API call");
  }

  virtual void inputsAvailable(BuildSystemCommandInterface& bsci,
                               core::Task* task) override {
    // If the build should cancel, do nothing.
    if (bsci.getDelegate().isCancelled()) {
      bsci.taskIsComplete(task, BuildValue::makeCancelledCommand());
      return;
    }
    
    auto fn = [this, &bsci=bsci, task](QueueJobContext* context) {
      // Notify the client the actual command body is going to run.
      bsci.getDelegate().commandStarted(this);

      // Create the directory containing the symlink, if necessary.
      //
      // FIXME: Shared behavior with ExternalCommand.
      //
      // FIXME: Need to use the filesystem interfaces.
      {
        auto parent = llvm::sys::path::parent_path(output->getName());
        if (!parent.empty()) {
          (void) llvm::sys::fs::create_directories(parent);
        }
      }

      // Create the symbolic link (note that despite the poorly chosen LLVM
      // name, this is a symlink).
      //
      // FIXME: Need to use the filesystem interfaces.
      auto success = true;
      if (llvm::sys::fs::create_link(contents, output->getName())) {
        // On failure, we attempt to unlink the file and retry.
        basic::sys::unlink(output->getName().str().c_str());
        
        if (llvm::sys::fs::create_link(contents, output->getName())) {
          getBuildSystem(bsci.getBuildEngine()).error(
              "", "unable to create symlink at '" + output->getName() + "'");
          success = false;
        }
      }
      
      // Notify the client the command is complete.
      bsci.getDelegate().commandFinished(this);
    
      // Process the result.
      if (!success) {
        bsci.getDelegate().hadCommandFailure();
        bsci.taskIsComplete(task, BuildValue::makeFailedCommand());
        return;
      }

      // Capture the *link* information of the output.
      FileInfo outputInfo = output->getLinkInfo(
          bsci.getDelegate().getFileSystem());
      
      // Complete with a successful result.
      bsci.taskIsComplete(
          task, BuildValue::makeSuccessfulCommand(outputInfo, getSignature()));
    };
    bsci.addJob({ this, std::move(fn) });
  }

public:
  using Command::Command;
};

class SymlinkTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<SymlinkCommand>(name);
  }
};

#pragma mark - ArchiveTool implementation

class ArchiveShellCommand : public ExternalCommand {

  std::string archiveName;
  std::vector<std::string> archiveInputs;

  virtual CommandResult executeExternalCommand(BuildSystemCommandInterface& bsci,
                                               Task* task,
                                               QueueJobContext* context) override {
    // First delete the current archive
    // TODO instead insert, update and remove files from the archive
    if (llvm::sys::fs::remove(archiveName, /*IgnoreNonExisting*/ true)) {
      return CommandResult::Failed;
    }

    // Create archive
    auto args = getArgs();
    return bsci.getExecutionQueue().executeProcess(context, std::vector<StringRef>(args.begin(), args.end()));
  }

  virtual void getShortDescription(SmallVectorImpl<char> &result) override {
    auto desc = getDescription();
    if (desc.empty()) {
      desc = "Archiving " + archiveName;
    }
    llvm::raw_svector_ostream(result) << desc;
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) override {
    llvm::raw_svector_ostream stream(result);
    bool first = true;
    for (const auto& arg: getArgs()) {
      stream << arg;
      if (!first) {
        stream << " ";
        first = false;
      }
    }
  }
  
  virtual void configureInputs(const ConfigureContext& ctx,
                                const std::vector<Node*>& value) override {
    ExternalCommand::configureInputs(ctx, value);

    for (const auto& input: getInputs()) {
      if (!input->isVirtual()) {
        archiveInputs.push_back(input->getName());
      }
    }
    if (archiveInputs.empty()) {
      ctx.error("missing expected input");
    }
  }
  
  virtual void configureOutputs(const ConfigureContext& ctx,
                                const std::vector<Node*>& value) override {
    ExternalCommand::configureOutputs(ctx, value);

    for (const auto& output: getOutputs()) {
      if (!output->isVirtual()) {
        if (archiveName.empty()) {
          archiveName = output->getName();
        } else {
          ctx.error("unexpected explicit output: " + output->getName());
        }
      }
    }
    if (archiveName.empty()) {
      ctx.error("missing expected output");
    }
  }

  std::vector<std::string> getArgs() {
    std::vector<std::string> args;
    args.push_back("ar");
    args.push_back("cr");
    args.push_back(archiveName);
    args.insert(args.end(), archiveInputs.begin(), archiveInputs.end());
    return args;
  }

public:
  using ExternalCommand::ExternalCommand;
};

class ArchiveTool : public Tool {
public:
  using Tool::Tool;

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // No supported attributes.
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    return llvm::make_unique<ArchiveShellCommand>(name);
  }
};

#pragma mark - BuildSystemFileDelegate

BuildSystemDelegate& BuildSystemFileDelegate::getSystemDelegate() {
  return system.getDelegate();
}

void BuildSystemFileDelegate::setFileContentsBeingParsed(StringRef buffer) {
  getSystemDelegate().setFileContentsBeingParsed(buffer);
}

void BuildSystemFileDelegate::error(StringRef filename,
                                    const BuildFileToken& at,
                                    const Twine& message) {
  // Delegate to the system delegate.
  auto atSystemToken = BuildSystemDelegate::Token{at.start, at.length};
  system.error(filename, atSystemToken, message);
}

bool
BuildSystemFileDelegate::configureClient(const ConfigureContext&,
                                         StringRef name,
                                         uint32_t version,
                                         const property_list_type& properties) {
  // The client must match the configured name of the build system.
  if (name != getSystemDelegate().getName())
    return false;

  // The client version must match the configured version.
  //
  // FIXME: We should give the client the opportunity to support a previous
  // schema version (auto-upgrade).
  if (version != getSystemDelegate().getVersion())
    return false;

  return true;
}

std::unique_ptr<Tool>
BuildSystemFileDelegate::lookupTool(StringRef name) {
  // First, give the client an opportunity to create the tool.
  if (auto tool = getSystemDelegate().lookupTool(name)) {
    return tool;
  }

  // Otherwise, look for one of the builtin tool definitions.
  if (name == "shell") {
    return llvm::make_unique<ShellTool>(name);
  } else if (name == "phony") {
    return llvm::make_unique<PhonyTool>(name);
  } else if (name == "clang") {
    return llvm::make_unique<ClangTool>(name);
  } else if (name == "mkdir") {
    return llvm::make_unique<MkdirTool>(name);
  } else if (name == "symlink") {
    return llvm::make_unique<SymlinkTool>(name);
  } else if (name == "archive") {
    return llvm::make_unique<ArchiveTool>(name);
  }

  return nullptr;
}

void BuildSystemFileDelegate::loadedTarget(StringRef name,
                                           const Target& target) {
}

void BuildSystemFileDelegate::loadedDefaultTarget(StringRef target) {
}

void BuildSystemFileDelegate::loadedCommand(StringRef name,
                                            const Command& command) {
}

std::unique_ptr<Node>
BuildSystemFileDelegate::lookupNode(StringRef name,
                                    bool isImplicit) {
  return system.lookupNode(name, isImplicit);
}

}

#pragma mark - BuildSystem

BuildSystem::BuildSystem(BuildSystemDelegate& delegate)
    : impl(new BuildSystemImpl(*this, delegate))
{
}

BuildSystem::~BuildSystem() {
  delete static_cast<BuildSystemImpl*>(impl);
}

BuildSystemDelegate& BuildSystem::getDelegate() {
  return static_cast<BuildSystemImpl*>(impl)->getDelegate();
}

bool BuildSystem::loadDescription(StringRef mainFilename) {
  return static_cast<BuildSystemImpl*>(impl)->loadDescription(mainFilename);
}

void BuildSystem::loadDescription(
    std::unique_ptr<BuildDescription> description) {
  return static_cast<BuildSystemImpl*>(impl)->loadDescription(
      std::move(description));
}

bool BuildSystem::attachDB(StringRef path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->attachDB(path, error_out);
}

bool BuildSystem::enableTracing(StringRef path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->enableTracing(path, error_out);
}

llvm::Optional<BuildValue> BuildSystem::build(BuildKey key) {
  return static_cast<BuildSystemImpl*>(impl)->build(key);
}

bool BuildSystem::build(StringRef name) {
  return static_cast<BuildSystemImpl*>(impl)->build(name);
}

void BuildSystem::cancel() {
  if (impl) {
    auto buildSystemImpl = static_cast<BuildSystemImpl*>(impl);
    buildSystemImpl->getCommandInterface().getExecutionQueue().cancelAllJobs();
  }
}
