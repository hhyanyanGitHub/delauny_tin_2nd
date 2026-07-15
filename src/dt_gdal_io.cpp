#include "dt_gdal_io.hpp"

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dt {
namespace {

struct DatasetCloser {
    void operator()(GDALDataset* dataset) const noexcept {
        if (dataset) GDALClose(dataset);
    }
};
using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;

struct FeatureCloser {
    void operator()(OGRFeature* feature) const noexcept {
        if (feature) OGRFeature::DestroyFeature(feature);
    }
};
using FeaturePtr = std::unique_ptr<OGRFeature, FeatureCloser>;

std::once_flag g_gdal_once;

void configure_proj_data() {
    if (CPLGetConfigOption("PROJ_DATA", nullptr) ||
        CPLGetConfigOption("PROJ_LIB", nullptr)) {
        return;
    }
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_gdal_once), &module)) {
        return;
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD length =
        GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return;
    const std::filesystem::path module_dir =
        std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
    for (const auto& candidate :
         {module_dir / "share" / "proj", module_dir / "proj",
          module_dir.parent_path() / "share" / "proj"}) {
        if (std::filesystem::exists(candidate / "proj.db")) {
            const std::string utf8 = candidate.u8string();
            CPLSetConfigOption("PROJ_DATA", utf8.c_str());
            return;
        }
    }
#endif
}

void require_name(const char* value, const char* field) {
    if (!value || *value == '\0') {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        std::string(field) + " is empty");
    }
}

[[noreturn]] void throw_gdal(const char* operation,
                             dt_status status = DT_E_IO) {
    const char* detail = CPLGetLastErrorMsg();
    std::string message = operation;
    if (detail && *detail) {
        message += ": ";
        message += detail;
    }
    throw Exception(status, message);
}

GDALDriver* require_driver(const char* name) {
    require_name(name, "GDAL driver name");
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName(name);
    if (!driver) {
        throw Exception(DT_E_UNSUPPORTED,
                        std::string("GDAL driver is unavailable: ") + name);
    }
    return driver;
}

void set_dataset_spatial_metadata(GDALDataset& dataset, const Grid& grid) {
    const double* node = grid.transform();
    double pixel[6] = {
        node[0] - 0.5 * node[1] - 0.5 * node[2], node[1], node[2],
        node[3] - 0.5 * node[4] - 0.5 * node[5], node[4], node[5]};
    if (dataset.SetGeoTransform(pixel) != CE_None) {
        throw_gdal("cannot write raster geotransform");
    }
    if (!grid.crs_wkt().empty() &&
        dataset.SetProjection(grid.crs_wkt().c_str()) != CE_None) {
        throw_gdal("cannot write raster CRS");
    }
}

void write_grid_values(GDALDataset& dataset, const Grid& grid) {
    GDALRasterBand* band = dataset.GetRasterBand(1);
    if (!band) throw_gdal("cannot access output raster band");
    if ((grid.flags() & DT_GRID_HAS_NODATA) != 0 &&
        band->SetNoDataValue(grid.nodata()) != CE_None) {
        throw_gdal("cannot write raster NoData value");
    }
    const int width = static_cast<int>(grid.width());
    for (uint64_t row = 0; row < grid.height(); ++row) {
        double* values = const_cast<double*>(
            grid.values().data() + static_cast<size_t>(row * grid.width()));
        if (band->RasterIO(GF_Write, 0, static_cast<int>(row), width, 1,
                           values, width, 1, GDT_Float64, 0, 0,
                           nullptr) != CE_None) {
            throw_gdal("cannot write raster values");
        }
    }
}

DatasetPtr create_memory_raster(const Grid& grid) {
    GDALDriver* driver = require_driver("MEM");
    DatasetPtr dataset(driver->Create("", static_cast<int>(grid.width()),
                                      static_cast<int>(grid.height()), 1,
                                      GDT_Float64, nullptr));
    if (!dataset) throw_gdal("cannot create temporary GDAL raster");
    set_dataset_spatial_metadata(*dataset, grid);
    write_grid_values(*dataset, grid);
    return dataset;
}

std::string spatial_reference_wkt(const OGRSpatialReference* spatial_ref) {
    if (!spatial_ref) return {};
    char* text = nullptr;
    if (spatial_ref->exportToWkt(&text) != OGRERR_NONE || !text) {
        throw_gdal("cannot export layer CRS", DT_E_CORRUPTED_DATA);
    }
    std::string result(text);
    CPLFree(text);
    return result;
}

void append_line(ContourSet& output, const OGRLineString& geometry,
                 double elevation, int closed_field) {
    const int count = geometry.getNumPoints();
    if (count < 2) return;
    ContourLine line;
    line.elevation = elevation;
    line.flags = (closed_field != 0 || geometry.get_IsClosed())
                     ? DT_CONTOUR_LINE_CLOSED
                     : 0;
    line.points.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double z = geometry.getCoordinateDimension() >= 3
                             ? geometry.getZ(i)
                             : elevation;
        line.points.push_back({geometry.getX(i), geometry.getY(i), z});
    }
    output.lines.push_back(std::move(line));
}

} // namespace

void gdal_initialize() {
    std::call_once(g_gdal_once, [] {
        configure_proj_data();
        GDALAllRegister();
    });
}

bool gdal_driver_available(const char* driver_name) {
    gdal_initialize();
    require_name(driver_name, "GDAL driver name");
    return GetGDALDriverManager()->GetDriverByName(driver_name) != nullptr;
}

std::unique_ptr<Grid> grid_load_gdal(
    const char* file_name, const dt_gdal_raster_load_options& options) {
    gdal_initialize();
    require_name(file_name, "raster file name");
    CPLErrorReset();
    DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
        file_name, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr,
        nullptr)));
    if (!dataset) throw_gdal("cannot open GDAL raster");
    const int width = dataset->GetRasterXSize();
    const int height = dataset->GetRasterYSize();
    if (width <= 0 || height <= 0) {
        throw Exception(DT_E_CORRUPTED_DATA, "GDAL raster has invalid size");
    }
    const uint32_t band_index = options.band_index == 0 ? 1 : options.band_index;
    if (band_index > static_cast<uint32_t>(dataset->GetRasterCount())) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "GDAL raster band index is out of range");
    }
    GDALRasterBand* band = dataset->GetRasterBand(static_cast<int>(band_index));
    if (!band) throw_gdal("cannot access GDAL raster band");

    double pixel[6] = {0, 1, 0, 0, 0, 1};
    CPLErrorReset();
    dataset->GetGeoTransform(pixel);
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = static_cast<uint64_t>(width);
    create.height = static_cast<uint64_t>(height);
    create.geo_transform[0] = pixel[0] + 0.5 * pixel[1] + 0.5 * pixel[2];
    create.geo_transform[1] = pixel[1];
    create.geo_transform[2] = pixel[2];
    create.geo_transform[3] = pixel[3] + 0.5 * pixel[4] + 0.5 * pixel[5];
    create.geo_transform[4] = pixel[4];
    create.geo_transform[5] = pixel[5];
    int has_nodata = 0;
    create.nodata_value = band->GetNoDataValue(&has_nodata);
    if (has_nodata) create.flags |= DT_GRID_HAS_NODATA;
    auto output = std::make_unique<Grid>(create);
    if (const char* projection = dataset->GetProjectionRef();
        projection && *projection) {
        output->set_crs_wkt(projection);
    }
    std::vector<double> row(static_cast<size_t>(width));
    for (int y = 0; y < height; ++y) {
        if (band->RasterIO(GF_Read, 0, y, width, 1, row.data(), width, 1,
                           GDT_Float64, 0, 0, nullptr) != CE_None) {
            throw_gdal("cannot read GDAL raster values",
                       DT_E_CORRUPTED_DATA);
        }
        output->write_window(0, static_cast<uint64_t>(y),
                             static_cast<uint64_t>(width), 1, row.data(),
                             static_cast<uint64_t>(width));
    }
    return output;
}

void grid_save_gdal(const Grid& grid, const char* file_name,
                    const dt_gdal_raster_save_options& options) {
    gdal_initialize();
    require_name(file_name, "raster file name");
    const char* driver_name = options.driver_name && *options.driver_name
                                  ? options.driver_name
                                  : "GTiff";
    GDALDriver* driver = require_driver(driver_name);
    CPLErrorReset();
    if (std::string(driver_name) == "COG") {
        auto source = create_memory_raster(grid);
        DatasetPtr output(driver->CreateCopy(
            file_name, source.get(), FALSE, options.creation_options, nullptr,
            nullptr));
        if (!output) throw_gdal("cannot create COG raster");
        return;
    }
    DatasetPtr output(driver->Create(
        file_name, static_cast<int>(grid.width()),
        static_cast<int>(grid.height()), 1, GDT_Float64,
        options.creation_options));
    if (!output) throw_gdal("cannot create GDAL raster");
    set_dataset_spatial_metadata(*output, grid);
    write_grid_values(*output, grid);
    if (output->FlushCache() != CE_None) {
        throw_gdal("cannot flush GDAL raster");
    }
}

std::unique_ptr<ContourSet> contours_load_gdal(
    const char* file_name, const dt_gdal_contour_load_options& options) {
    gdal_initialize();
    require_name(file_name, "vector file name");
    CPLErrorReset();
    DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
        file_name, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
        nullptr)));
    if (!dataset) throw_gdal("cannot open GDAL vector dataset");
    OGRLayer* layer = options.layer_name && *options.layer_name
                          ? dataset->GetLayerByName(options.layer_name)
                          : dataset->GetLayer(0);
    if (!layer) throw Exception(DT_E_NOT_FOUND, "contour layer was not found");
    const char* elevation_name =
        options.elevation_field && *options.elevation_field
            ? options.elevation_field
            : "elevation";
    const int elevation_index =
        layer->GetLayerDefn()->GetFieldIndex(elevation_name);
    const int closed_index = layer->GetLayerDefn()->GetFieldIndex("closed");
    auto output = std::make_unique<ContourSet>();
    output->crs_wkt = spatial_reference_wkt(layer->GetSpatialRef());
    layer->ResetReading();
    while (FeaturePtr feature{layer->GetNextFeature()}) {
        OGRGeometry* geometry = feature->GetGeometryRef();
        if (!geometry || geometry->IsEmpty()) continue;
        const int closed = closed_index >= 0
                               ? feature->GetFieldAsInteger(closed_index)
                               : 0;
        auto process = [&](const OGRLineString& line) {
            double elevation = 0.0;
            if (elevation_index >= 0 &&
                feature->IsFieldSetAndNotNull(elevation_index)) {
                elevation = feature->GetFieldAsDouble(elevation_index);
            } else if (line.getCoordinateDimension() >= 3 &&
                       line.getNumPoints() != 0) {
                elevation = line.getZ(0);
            } else {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "contour feature has no elevation");
            }
            append_line(*output, line, elevation, closed);
        };
        const OGRwkbGeometryType type = wkbFlatten(geometry->getGeometryType());
        if (type == wkbLineString) {
            process(*geometry->toLineString());
        } else if (type == wkbMultiLineString) {
            const auto* multi = geometry->toMultiLineString();
            for (const auto* part : *multi) process(*part);
        }
    }
    return output;
}

void contours_save_gdal(const ContourSet& contours, const char* file_name,
                        const dt_gdal_contour_save_options& options) {
    gdal_initialize();
    require_name(file_name, "vector file name");
    const char* driver_name = options.driver_name && *options.driver_name
                                  ? options.driver_name
                                  : "GPKG";
    const char* layer_name = options.layer_name && *options.layer_name
                                 ? options.layer_name
                                 : "contours";
    const char* elevation_name =
        options.elevation_field && *options.elevation_field
            ? options.elevation_field
            : "elevation";
    GDALDriver* driver = require_driver(driver_name);
    CPLErrorReset();
    DatasetPtr dataset(driver->Create(file_name, 0, 0, 0, GDT_Unknown,
                                      options.dataset_creation_options));
    if (!dataset) throw_gdal("cannot create GDAL vector dataset");
    OGRSpatialReference spatial_ref;
    OGRSpatialReference* spatial_ref_ptr = nullptr;
    if (!contours.crs_wkt.empty()) {
        if (spatial_ref.SetFromUserInput(contours.crs_wkt.c_str()) !=
            OGRERR_NONE) {
            throw Exception(DT_E_INVALID_ARGUMENT, "invalid contour CRS WKT");
        }
        spatial_ref_ptr = &spatial_ref;
    }
    OGRLayer* layer = dataset->CreateLayer(
        layer_name, spatial_ref_ptr, wkbLineString25D,
        options.layer_creation_options);
    if (!layer) throw_gdal("cannot create contour layer");
    OGRFieldDefn elevation_field(elevation_name, OFTReal);
    if (layer->CreateField(&elevation_field) != OGRERR_NONE) {
        throw_gdal("cannot create contour elevation field");
    }
    OGRFieldDefn closed_field("closed", OFTInteger);
    if (layer->CreateField(&closed_field) != OGRERR_NONE) {
        throw_gdal("cannot create contour closed field");
    }
    if (layer->StartTransaction() != OGRERR_NONE) {
        throw_gdal("cannot start contour transaction");
    }
    try {
        for (const auto& source : contours.lines) {
            OGRLineString geometry;
            geometry.setCoordinateDimension(3);
            for (const auto& point : source.points) {
                geometry.addPoint(point.x, point.y, point.z);
            }
            FeaturePtr feature{
                OGRFeature::CreateFeature(layer->GetLayerDefn())};
            if (!feature) throw_gdal("cannot allocate contour feature");
            feature->SetField(elevation_name, source.elevation);
            feature->SetField("closed",
                              (source.flags & DT_CONTOUR_LINE_CLOSED) != 0);
            if (feature->SetGeometry(&geometry) != OGRERR_NONE ||
                layer->CreateFeature(feature.get()) != OGRERR_NONE) {
                throw_gdal("cannot write contour feature");
            }
        }
        if (layer->CommitTransaction() != OGRERR_NONE) {
            throw_gdal("cannot commit contour transaction");
        }
    } catch (...) {
        layer->RollbackTransaction();
        throw;
    }
}

} // namespace dt
