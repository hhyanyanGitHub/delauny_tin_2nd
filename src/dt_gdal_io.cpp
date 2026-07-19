#include "dt_gdal_io.hpp"

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal_priv.h>
#include <gdalwarper.h>
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

struct CoordinateTransformationCloser {
    void operator()(OGRCoordinateTransformation* transformation) const noexcept {
        if (transformation) OCTDestroyCoordinateTransformation(transformation);
    }
};
using CoordinateTransformationPtr =
    std::unique_ptr<OGRCoordinateTransformation,
                    CoordinateTransformationCloser>;

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

std::unique_ptr<OGRSpatialReference> parse_crs(const char* definition,
                                                const char* field) {
    require_name(definition, field);
    auto reference = std::make_unique<OGRSpatialReference>();
    if (reference->SetFromUserInput(definition) != OGRERR_NONE) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        std::string("invalid ") + field + ": " + definition);
    }
    reference->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    return reference;
}

CoordinateTransformationPtr make_transformation(
    const char* source_definition, const char* target_definition,
    std::unique_ptr<OGRSpatialReference>& source,
    std::unique_ptr<OGRSpatialReference>& target) {
    source = parse_crs(source_definition, "source CRS");
    target = parse_crs(target_definition, "target CRS");
    CoordinateTransformationPtr transformation(
        OGRCreateCoordinateTransformation(source.get(), target.get()));
    if (!transformation) throw_gdal("cannot create CRS transformation",
                                    DT_E_INVALID_ARGUMENT);
    return transformation;
}

void transform_point_vector(OGRCoordinateTransformation& transformation,
                            std::vector<dt_point3>& points) {
    constexpr size_t kBatch = 65536;
    std::vector<double> x(kBatch), y(kBatch), z(kBatch);
    std::vector<int> success(kBatch);
    for (size_t base = 0; base < points.size(); base += kBatch) {
        const size_t count = std::min(kBatch, points.size() - base);
        for (size_t i = 0; i < count; ++i) {
            const dt_point3& point = points[base + i];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "CRS transformation input is not finite");
            }
            x[i] = point.x;
            y[i] = point.y;
            z[i] = point.z;
            success[i] = FALSE;
        }
        if (!transformation.Transform(static_cast<int>(count), x.data(),
                                      y.data(), z.data(), success.data())) {
            throw_gdal("CRS coordinate transformation failed",
                       DT_E_INVALID_ARGUMENT);
        }
        for (size_t i = 0; i < count; ++i) {
            if (!success[i] || !std::isfinite(x[i]) || !std::isfinite(y[i]) ||
                !std::isfinite(z[i])) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "CRS coordinate transformation failed for a point");
            }
            points[base + i] = {x[i], y[i], z[i]};
        }
    }
}

GDALResampleAlg resample_algorithm(uint32_t value) {
    switch (value == 0 ? static_cast<uint32_t>(DT_GDAL_RESAMPLE_BILINEAR)
                       : value) {
    case DT_GDAL_RESAMPLE_NEAREST: return GRA_NearestNeighbour;
    case DT_GDAL_RESAMPLE_BILINEAR: return GRA_Bilinear;
    case DT_GDAL_RESAMPLE_CUBIC: return GRA_Cubic;
    case DT_GDAL_RESAMPLE_CUBIC_SPLINE: return GRA_CubicSpline;
    case DT_GDAL_RESAMPLE_LANCZOS: return GRA_Lanczos;
    default:
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "unsupported GDAL reprojection resample algorithm");
    }
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

std::string normalize_crs_wkt(const char* definition) {
    gdal_initialize();
    auto reference = parse_crs(definition, "CRS definition");
    char* text = nullptr;
    if (reference->exportToWkt(&text) != OGRERR_NONE || !text) {
        throw_gdal("cannot export normalized CRS", DT_E_INVALID_ARGUMENT);
    }
    std::string result(text);
    CPLFree(text);
    return result;
}

bool crs_is_same(const char* first, const char* second) {
    gdal_initialize();
    auto first_reference = parse_crs(first, "first CRS");
    auto second_reference = parse_crs(second, "second CRS");
    const char* options[]{
        "CRITERION=EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS", nullptr};
    if (first_reference->IsSame(second_reference.get(), options) != FALSE)
        return true;
    return first_reference->IsGeographic() != FALSE &&
           second_reference->IsGeographic() != FALSE &&
           first_reference->IsSameGeogCS(second_reference.get()) != FALSE;
}

void transform_points(const char* source_crs, const char* target_crs,
                      const dt_point3* input, uint64_t count,
                      dt_point3* output) {
    gdal_initialize();
    if (count != 0 && (!input || !output)) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "CRS point input/output array is null");
    }
    if (count > static_cast<uint64_t>(
                    std::numeric_limits<size_t>::max() / sizeof(dt_point3))) {
        throw Exception(DT_E_LIMIT_EXCEEDED,
                        "CRS point count exceeds addressable memory");
    }
    std::unique_ptr<OGRSpatialReference> source_reference;
    std::unique_ptr<OGRSpatialReference> target_reference;
    auto transformation = make_transformation(
        source_crs, target_crs, source_reference, target_reference);
    std::vector<dt_point3> transformed;
    if (count != 0) transformed.assign(input, input + count);
    transform_point_vector(*transformation, transformed);
    if (count != 0) {
        std::memmove(output, transformed.data(),
                     static_cast<size_t>(count) * sizeof(dt_point3));
    }
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

std::unique_ptr<Grid> grid_reproject_gdal(
    const Grid& source, const dt_gdal_reproject_options& options) {
    gdal_initialize();
    if (source.crs_wkt().empty()) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "source GRID has no CRS metadata");
    }
    const std::string target_wkt = normalize_crs_wkt(options.target_crs);
    auto source_dataset = create_memory_raster(source);
    double output_pixel[6]{};
    int output_width = 0;
    int output_height = 0;
    if ((options.flags & DT_GDAL_REPROJECT_EXPLICIT_GRID) != 0) {
        if (options.width == 0 || options.height == 0 ||
            options.width > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
            options.height > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "explicit reprojection GRID size is invalid");
        }
        for (double value : options.geo_transform) {
            if (!std::isfinite(value)) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "explicit reprojection affine is not finite");
            }
        }
        const double determinant =
            options.geo_transform[1] * options.geo_transform[5] -
            options.geo_transform[2] * options.geo_transform[4];
        if (std::abs(determinant) <= 1.0e-18) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "explicit reprojection affine is singular");
        }
        output_width = static_cast<int>(options.width);
        output_height = static_cast<int>(options.height);
        output_pixel[0] = options.geo_transform[0] -
                          0.5 * options.geo_transform[1] -
                          0.5 * options.geo_transform[2];
        output_pixel[1] = options.geo_transform[1];
        output_pixel[2] = options.geo_transform[2];
        output_pixel[3] = options.geo_transform[3] -
                          0.5 * options.geo_transform[4] -
                          0.5 * options.geo_transform[5];
        output_pixel[4] = options.geo_transform[4];
        output_pixel[5] = options.geo_transform[5];
    } else {
        void* transformer = GDALCreateGenImgProjTransformer(
            source_dataset.get(), source.crs_wkt().c_str(), nullptr,
            target_wkt.c_str(), FALSE, 0.0, 1);
        if (!transformer) throw_gdal("cannot prepare GRID reprojection",
                                     DT_E_INVALID_ARGUMENT);
        const CPLErr suggested = GDALSuggestedWarpOutput(
            source_dataset.get(), GDALGenImgProjTransform, transformer,
            output_pixel, &output_width, &output_height);
        GDALDestroyGenImgProjTransformer(transformer);
        if (suggested != CE_None || output_width <= 0 || output_height <= 0) {
            throw_gdal("cannot derive reprojection output grid",
                       DT_E_INVALID_ARGUMENT);
        }
    }

    GDALDriver* memory_driver = require_driver("MEM");
    DatasetPtr output_dataset(memory_driver->Create(
        "", output_width, output_height, 1, GDT_Float64, nullptr));
    if (!output_dataset) throw_gdal("cannot create reprojection output GRID");
    if (output_dataset->SetGeoTransform(output_pixel) != CE_None ||
        output_dataset->SetProjection(target_wkt.c_str()) != CE_None) {
        throw_gdal("cannot configure reprojection output GRID");
    }
    const double output_nodata =
        options.output_nodata_value != 0.0
            ? options.output_nodata_value
            : ((source.flags() & DT_GRID_HAS_NODATA) != 0
                   ? source.nodata()
                   : std::numeric_limits<double>::quiet_NaN());
    GDALRasterBand* output_band = output_dataset->GetRasterBand(1);
    if (!output_band || output_band->SetNoDataValue(output_nodata) != CE_None ||
        output_band->Fill(output_nodata) != CE_None) {
        throw_gdal("cannot initialize reprojection output values");
    }
    CPLErrorReset();
    if (GDALReprojectImage(
            source_dataset.get(), source.crs_wkt().c_str(),
            output_dataset.get(), target_wkt.c_str(),
            resample_algorithm(options.resample_algorithm), 0.0, 0.0,
            nullptr, nullptr, nullptr) != CE_None) {
        throw_gdal("GRID reprojection failed", DT_E_INVALID_ARGUMENT);
    }

    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = DT_GRID_HAS_NODATA;
    create.width = static_cast<uint64_t>(output_width);
    create.height = static_cast<uint64_t>(output_height);
    create.geo_transform[0] = output_pixel[0] +
                              0.5 * output_pixel[1] +
                              0.5 * output_pixel[2];
    create.geo_transform[1] = output_pixel[1];
    create.geo_transform[2] = output_pixel[2];
    create.geo_transform[3] = output_pixel[3] +
                              0.5 * output_pixel[4] +
                              0.5 * output_pixel[5];
    create.geo_transform[4] = output_pixel[4];
    create.geo_transform[5] = output_pixel[5];
    create.nodata_value = output_nodata;
    auto output = std::make_unique<Grid>(create);
    output->set_crs_wkt(target_wkt);
    std::vector<double> row(static_cast<size_t>(output_width));
    for (int y = 0; y < output_height; ++y) {
        if (output_band->RasterIO(GF_Read, 0, y, output_width, 1,
                                  row.data(), output_width, 1, GDT_Float64,
                                  0, 0, nullptr) != CE_None) {
            throw_gdal("cannot read reprojected GRID", DT_E_CORRUPTED_DATA);
        }
        output->write_window(0, static_cast<uint64_t>(y),
                             static_cast<uint64_t>(output_width), 1,
                             row.data(), static_cast<uint64_t>(output_width));
    }
    return output;
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

std::unique_ptr<ContourSet> contours_reproject_gdal(
    const ContourSet& source, const char* target_crs) {
    gdal_initialize();
    if (source.crs_wkt.empty()) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "source contours have no CRS metadata");
    }
    std::unique_ptr<OGRSpatialReference> source_reference;
    std::unique_ptr<OGRSpatialReference> target_reference;
    auto transformation = make_transformation(
        source.crs_wkt.c_str(), target_crs, source_reference,
        target_reference);
    auto output = std::make_unique<ContourSet>(source);
    for (ContourLine& line : output->lines) {
        transform_point_vector(*transformation, line.points);
        if (!line.points.empty()) line.elevation = line.points.front().z;
    }
    char* target_wkt = nullptr;
    if (target_reference->exportToWkt(&target_wkt) != OGRERR_NONE ||
        !target_wkt) {
        throw_gdal("cannot export target contour CRS", DT_E_INVALID_ARGUMENT);
    }
    output->crs_wkt = target_wkt;
    CPLFree(target_wkt);
    return output;
}

} // namespace dt
