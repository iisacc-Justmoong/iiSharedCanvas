#pragma once
#include <Core/RasterBlendMode.h>
#include <array>
#include <optional>
#include <string_view>
#include <utility>
namespace iiSharedCanvas::layered_detail {
inline constexpr std::array<std::pair<RasterBlendMode, std::string_view>,27> PsdBlendModes{{
    {RasterBlendMode::SourceOver,"norm"}, {RasterBlendMode::Dissolve,"diss"}, {RasterBlendMode::Darken,"dark"},
    {RasterBlendMode::Multiply,"mul "}, {RasterBlendMode::ColorBurn,"idiv"}, {RasterBlendMode::LinearBurn,"lbrn"},
    {RasterBlendMode::DarkerColor,"dkCl"}, {RasterBlendMode::Lighten,"lite"}, {RasterBlendMode::Screen,"scrn"},
    {RasterBlendMode::ColorDodge,"div "}, {RasterBlendMode::LinearDodge,"lddg"}, {RasterBlendMode::LighterColor,"lgCl"},
    {RasterBlendMode::Overlay,"over"}, {RasterBlendMode::SoftLight,"sLit"}, {RasterBlendMode::HardLight,"hLit"},
    {RasterBlendMode::VividLight,"vLit"}, {RasterBlendMode::LinearLight,"lLit"}, {RasterBlendMode::PinLight,"pLit"},
    {RasterBlendMode::HardMix,"hMix"}, {RasterBlendMode::Difference,"diff"}, {RasterBlendMode::Exclusion,"smud"},
    {RasterBlendMode::Subtract,"fsub"}, {RasterBlendMode::Divide,"fdiv"}, {RasterBlendMode::Hue,"hue "},
    {RasterBlendMode::Saturation,"sat "}, {RasterBlendMode::Color,"colr"}, {RasterBlendMode::Luminosity,"lum "}
}};
inline constexpr std::array<std::pair<RasterBlendMode,std::string_view>,15> OraBlendModes{{
    {RasterBlendMode::SourceOver,"svg:src-over"}, {RasterBlendMode::Multiply,"svg:multiply"},
    {RasterBlendMode::Screen,"svg:screen"}, {RasterBlendMode::Overlay,"svg:overlay"},
    {RasterBlendMode::Darken,"svg:darken"}, {RasterBlendMode::Lighten,"svg:lighten"},
    {RasterBlendMode::ColorDodge,"svg:color-dodge"}, {RasterBlendMode::ColorBurn,"svg:color-burn"},
    {RasterBlendMode::HardLight,"svg:hard-light"}, {RasterBlendMode::SoftLight,"svg:soft-light"},
    {RasterBlendMode::Difference,"svg:difference"}, {RasterBlendMode::Color,"svg:color"},
    {RasterBlendMode::Luminosity,"svg:luminosity"}, {RasterBlendMode::Hue,"svg:hue"},
    {RasterBlendMode::Saturation,"svg:saturation"}
}};
}
