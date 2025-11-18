#include "usd_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include <pxr/usd/usd/primRange.h>
#include <filesystem>

namespace duckdb {

pxr::UsdStageRefPtr UsdStageManager::OpenStage(const std::string &file_path) {
    // Validate file exists
    if (!std::filesystem::exists(file_path)) {
        throw IOException("USD file not found: " + file_path);
    }
    
    // Open the USD stage
    auto stage = pxr::UsdStage::Open(file_path);
    if (!stage) {
        throw IOException("Failed to open USD stage: " + file_path);
    }
    
    return stage;
}

bool UsdStageManager::IsValidUsdFile(const std::string &file_path) {
    // Check for empty or whitespace-only paths
    if (file_path.empty() || file_path.find_first_not_of(" \t\n\r") == std::string::npos) {
        return false;
    }

    if (!std::filesystem::exists(file_path)) {
        return false;
    }

    // Check if it's a directory
    if (std::filesystem::is_directory(file_path)) {
        return false;
    }

    // Check file extension
    std::filesystem::path path(file_path);
    std::string ext = path.extension().string();

    // USD file extensions: .usd, .usda, .usdc, .usdz
    return (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz");
}

UsdPrimIterator::UsdPrimIterator(pxr::UsdStageRefPtr stage) 
    : stage_(stage), range_(stage->Traverse()) {
    current_ = range_.begin();
    end_ = range_.end();
}

bool UsdPrimIterator::HasNext() const {
    return current_ != end_;
}

pxr::UsdPrim UsdPrimIterator::GetNext() {
    if (!HasNext()) {
        throw InternalException("UsdPrimIterator: No more prims available");
    }
    
    auto prim = *current_;
    ++current_;
    return prim;
}

void UsdPrimIterator::Reset() {
    current_ = range_.begin();
}

} // namespace duckdb

