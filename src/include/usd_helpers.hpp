#pragma once

#include "duckdb.hpp"
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <string>
#include <memory>

namespace duckdb {

class UsdStageManager {
public:
    static pxr::UsdStageRefPtr OpenStage(const std::string &file_path);
    static bool IsValidUsdFile(const std::string &file_path);
};

class UsdPrimIterator {
public:
    explicit UsdPrimIterator(pxr::UsdStageRefPtr stage);
    
    bool HasNext() const;
    pxr::UsdPrim GetNext();
    void Reset();
    
private:
    pxr::UsdStageRefPtr stage_;
    pxr::UsdPrimRange::iterator current_;
    pxr::UsdPrimRange::iterator end_;
    pxr::UsdPrimRange range_;
};

} // namespace duckdb

