#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/PipelineC/Manager.h"

// NOLINTBEGIN

struct rp_error_reason {
public:
  std::string Message;
  std::string Location;

  rp_error_reason(std::string Message, std::string Location) :
    Message(std::move(Message)), Location(std::move(Location)) {}
};

struct rp_document_error {
public:
  std::vector<rp_error_reason> Reasons;
  std::string LocationType;
  std::string ErrorType;

  rp_document_error(std::string ErrorType, std::string LocationType) :
    LocationType(std::move(LocationType)), ErrorType(std::move(ErrorType)) {}
};

struct rp_simple_error {
public:
  std::string Message;
  std::string ErrorType;

  rp_simple_error(std::string Message, std::string Type) :
    Message(std::move(Message)), ErrorType(std::move(Type)) {}
};

using rp_error = std::variant<std::monostate /* allows "no value" state */,
                              rp_simple_error,
                              rp_document_error>;

typedef revng::embed::Manager rp_manager;
class RawBinaryView;
typedef RawBinaryView rp_binary_view;

// `Kind` used to be an open hierarchy contributed to by each library; it is now
// a closed set of three values, so a handle is a pointer into a static table.
typedef const Kind rp_kind;

// A step is a point in the pipeline. Savepoints and artifacts both name one.
typedef const revng::runner::PipelineNode rp_step;

// A container is identified by its declaration, and reached at a step.
typedef const revng::runner::ContainerDeclaration rp_container_identifier;
typedef const revng::embed::ContainerHandle rp_container;

typedef const revng::embed::TargetHandle rp_target;
typedef const std::vector<revng::embed::TargetHandle> rp_targets_list;

typedef const ModelDiff rp_diff_map;
typedef llvm::StringMap<std::string> rp_string_map;

// Objects a change made stale, as `<savepoint>/<container>/<object>`.
typedef std::vector<std::string> rp_invalidations;

typedef llvm::SmallVector<char, 0> rp_buffer;
typedef revng::embed::ContainerTargetsMap rp_container_targets_map;

// NOLINTEND
