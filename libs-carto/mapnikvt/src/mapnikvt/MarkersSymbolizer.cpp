#include "MarkersSymbolizer.h"
#include "ParserUtils.h"
#include "vt/BitmapCanvas.h"

#include <boost/optional.hpp>
#include <boost/lexical_cast.hpp>

namespace carto { namespace mvt {
    void MarkersSymbolizer::build(const FeatureCollection& featureCollection, const FeatureExpressionContext& exprContext, const SymbolizerContext& symbolizerContext, vt::TileLayerBuilder& layerBuilder) {
        std::lock_guard<std::mutex> lock(_mutex);

        updateBindings(exprContext);

        vt::CompOp compOp = convertCompOp(_compOp);

        float fontScale = symbolizerContext.getSettings().getFontScale();
        vt::LabelOrientation placement = convertLabelPlacement(_placement);
        vt::LabelOrientation orientation = placement;
        if (_transformExpression) { // if rotation transform is explicitly defined, use point orientation
            if (containsRotationTransform(_transformExpression->evaluate(exprContext))) {
                orientation = vt::LabelOrientation::POINT;
            }
        }
        if (placement == vt::LabelOrientation::LINE && _spacing > 0) {
            orientation = vt::LabelOrientation::POINT; // we will apply custom rotation, thus use point orientation
        }

        float bitmapScaleX = fontScale, bitmapScaleY = fontScale;
        std::shared_ptr<const vt::Bitmap> bitmap;
        std::string file = _file;
        float fillOpacity = _fillOpacity;
        if (!file.empty()) {
            bitmap = symbolizerContext.getBitmapManager()->loadBitmap(file);
            if (!bitmap) {
                _logger->write(Logger::Severity::ERROR, "Failed to load marker bitmap " + file);
                return;
            }
            if (_width > 0) {
                bitmapScaleX = fontScale * _width / bitmap->width;
                bitmapScaleY = (_height > 0 ? fontScale * _height / bitmap->height : bitmapScaleX);
            }
            else if (_height > 0) {
                bitmapScaleX = bitmapScaleY = fontScale * _height / bitmap->height;
            }
        }
        else {
            vt::Color fill = vt::Color::fromColorOpacity(_fill, _fillOpacity);
            vt::Color stroke = vt::Color::fromColorOpacity(_stroke, _strokeOpacity);
            if (_markerType == "ellipse" || (_markerType.empty() && placement != vt::LabelOrientation::LINE)) {
                float width = DEFAULT_CIRCLE_SIZE, height = DEFAULT_CIRCLE_SIZE;
                if (_widthDefined) { // NOTE: special case, if accept all values
                    width = std::abs(_width);
                    height = (_heightDefined ? std::abs(_height) : width);
                }
                else if (_heightDefined) { // NOTE: special case, accept all values
                    height = std::abs(_height);
                    width = height;
                }
                file = "__default_marker_ellipse_" + boost::lexical_cast<std::string>(width) + "_" + boost::lexical_cast<std::string>(height) + "_" + boost::lexical_cast<std::string>(fill.value()) + "_" + boost::lexical_cast<std::string>(_strokeWidth) + "_" + boost::lexical_cast<std::string>(stroke.value()) + ".bmp";
                bitmap = symbolizerContext.getBitmapManager()->getBitmap(file);
                if (!bitmap) {
                    bitmap = makeEllipseBitmap(width * SUPERSAMPLING_FACTOR, height * SUPERSAMPLING_FACTOR, fill, std::abs(_strokeWidth) * SUPERSAMPLING_FACTOR, stroke);
                    symbolizerContext.getBitmapManager()->storeBitmap(file, bitmap);
                }
                bitmapScaleX = width  * fontScale / bitmap->width;
                bitmapScaleY = height * fontScale / bitmap->height;
            }
            else {
                float width = DEFAULT_ARROW_WIDTH, height = DEFAULT_ARROW_HEIGHT;
                if (_width > 0) {
                    width = _width;
                    height = (_height > 0 ? _height : DEFAULT_ARROW_HEIGHT * width / DEFAULT_ARROW_WIDTH);
                }
                else if (_height > 0) {
                    height = _height;
                    width = DEFAULT_ARROW_WIDTH * height / DEFAULT_ARROW_HEIGHT;
                }
                file = "__default_marker_arrow_" + boost::lexical_cast<std::string>(width) + "_" + boost::lexical_cast<std::string>(height) + "_" + boost::lexical_cast<std::string>(fill.value()) + "_" + boost::lexical_cast<std::string>(_strokeWidth) + "_" + boost::lexical_cast<std::string>(stroke.value()) + ".bmp";
                bitmap = symbolizerContext.getBitmapManager()->getBitmap(file);
                if (!bitmap) {
                    bitmap = makeArrowBitmap(width * SUPERSAMPLING_FACTOR, height * SUPERSAMPLING_FACTOR, fill, std::abs(_strokeWidth) * SUPERSAMPLING_FACTOR, stroke);
                    symbolizerContext.getBitmapManager()->storeBitmap(file, bitmap);
                }
                bitmapScaleX = width  * fontScale / bitmap->width;
                bitmapScaleY = height * fontScale / bitmap->height;
            }
            fillOpacity = 1.0f;
        }

        float bitmapSize = static_cast<float>(std::max(bitmap->width * bitmapScaleX, bitmap->height * bitmapScaleY));
        int groupId = (_allowOverlap ? -1 : 0);

        std::vector<std::pair<long long, vt::TileLayerBuilder::Vertex>> pointInfos;
        std::vector<std::pair<long long, vt::TileLayerBuilder::BitmapLabelInfo>> labelInfos;

        auto addPoint = [&](long long localId, long long globalId, const boost::variant<vt::TileLayerBuilder::Vertex, vt::TileLayerBuilder::Vertices>& position) {
            if (_allowOverlap) {
                if (auto vertex = boost::get<vt::TileLayerBuilder::Vertex>(&position)) {
                    pointInfos.emplace_back(localId, *vertex);
                }
                else if (auto vertices = boost::get<vt::TileLayerBuilder::Vertices>(&position)) {
                    if (!vertices->empty()) {
                        pointInfos.emplace_back(localId, vertices->front());
                    }
                }
            }
            else {
                labelInfos.emplace_back(localId, vt::TileLayerBuilder::BitmapLabelInfo(getBitmapId(globalId, file), groupId, position, 0));
            }
        };

        auto flushPoints = [&](const cglib::mat3x3<float>& transform) {
            if (_allowOverlap) {
                std::shared_ptr<const vt::ColorFunction> fillFunc = createColorFunction("#ffffff");
                std::shared_ptr<const vt::FloatFunction> opacityFunc = createFloatFunction(fillOpacity);

                vt::PointStyle style(compOp, convertLabelToPointOrientation(orientation), fillFunc, opacityFunc, symbolizerContext.getGlyphMap(), bitmap, transform * cglib::scale3_matrix(cglib::vec3<float>(bitmapScaleX, bitmapScaleY, 1)));

                std::size_t pointInfoIndex = 0;
                layerBuilder.addPoints([&](long long& id, vt::TileLayerBuilder::Vertex& vertex) {
                    if (pointInfoIndex >= pointInfos.size()) {
                        return false;
                    }
                    id = pointInfos[pointInfoIndex].first;
                    vertex = pointInfos[pointInfoIndex].second;
                    pointInfoIndex++;
                    return true;
                }, style);
                
                pointInfos.clear();
            }
            else {
                vt::BitmapLabelStyle style(orientation, vt::Color::fromColorOpacity(vt::Color(0xffffffff), fillOpacity), symbolizerContext.getFontManager()->getNullFont(), bitmap, transform * cglib::scale3_matrix(cglib::vec3<float>(bitmapScaleX, bitmapScaleY, 1)));

                std::size_t labelInfoIndex = 0;
                layerBuilder.addBitmapLabels([&](long long& id, vt::TileLayerBuilder::BitmapLabelInfo& labelInfo) {
                    if (labelInfoIndex >= labelInfos.size()) {
                        return false;
                    }
                    id = labelInfos[labelInfoIndex].first;
                    labelInfo = std::move(labelInfos[labelInfoIndex].second);
                    labelInfoIndex++;
                    return true;
                }, style);
                
                labelInfos.clear();
            }
        };

        for (std::size_t index = 0; index < featureCollection.getSize(); index++) {
            long long localId = featureCollection.getLocalId(index);
            long long globalId = featureCollection.getGlobalId(index);
            const std::shared_ptr<const Geometry>& geometry = featureCollection.getGeometry(index);

            if (auto pointGeometry = std::dynamic_pointer_cast<const PointGeometry>(geometry)) {
                for (const auto& vertex : pointGeometry->getVertices()) {
                    addPoint(localId, globalId, vertex);
                }
            }
            else if (auto lineGeometry = std::dynamic_pointer_cast<const LineGeometry>(geometry)) {
                if (placement == vt::LabelOrientation::LINE) {
                    for (const auto& vertices : lineGeometry->getVerticesList()) {
                        if (_spacing <= 0) {
                            addPoint(localId, globalId, vertices);
                            continue;
                        }

                        flushPoints(_transform); // NOTE: we need to flush previous points at this point as we will recalculate transform, which is part of the style

                        float linePos = 0;
                        for (std::size_t i = 1; i < vertices.size(); i++) {
                            const cglib::vec2<float>& v0 = vertices[i - 1];
                            const cglib::vec2<float>& v1 = vertices[i];

                            float lineLen = cglib::length(v1 - v0) * symbolizerContext.getSettings().getTileSize();
                            if (i == 1) {
                                linePos = std::min(lineLen, _spacing) * 0.5f;
                            }
                            while (linePos < lineLen) {
                                cglib::vec2<float> pos = v0 + (v1 - v0) * (linePos / lineLen);
                                if (std::min(pos(0), pos(1)) > 0.0f && std::max(pos(0), pos(1)) < 1.0f) {
                                    addPoint(localId, 0, pos);

                                    cglib::vec2<float> dir = cglib::unit(v1 - v0);
                                    cglib::mat3x3<float> dirTransform = cglib::mat3x3<float>::identity();
                                    dirTransform(0, 0) = dir(0);
                                    dirTransform(0, 1) = -dir(1);
                                    dirTransform(1, 0) = dir(1);
                                    dirTransform(1, 1) = dir(0);
                                    flushPoints(dirTransform * _transform); // NOTE: we should flush to be sure that the point will not get buffered
                                }

                                linePos += _spacing + bitmapSize;
                            }

                            linePos -= lineLen;
                        }
                    }
                }
                else {
                    for (const auto& vertex : lineGeometry->getMidPoints()) {
                        addPoint(localId, globalId, vertex);
                    }
                }
            }
            else if (auto polygonGeometry = std::dynamic_pointer_cast<const PolygonGeometry>(geometry)) {
                for (const auto& vertex : polygonGeometry->getSurfacePoints()) {
                    addPoint(localId, globalId, vertex);
                }
            }
            else {
                _logger->write(Logger::Severity::WARNING, "Unsupported geometry for MarkersSymbolizer");
            }
        }

        flushPoints(_transform);
    }

    void MarkersSymbolizer::bindParameter(const std::string& name, const std::string& value) {
        if (name == "file") {
            bind(&_file, parseStringExpression(value));
        }
        else if (name == "placement") {
            bind(&_placement, parseStringExpression(value));
        }
        else if (name == "marker-type") {
            bind(&_markerType, parseStringExpression(value));
        }
        else if (name == "fill") {
            bind(&_fill, parseStringExpression(value), &MarkersSymbolizer::convertColor);
        }
        else if (name == "fill-opacity") {
            bind(&_fillOpacity, parseExpression(value));
        }
        else if (name == "width") {
            bind(&_width, parseExpression(value));
            _widthDefined = true;
        }
        else if (name == "height") {
            bind(&_height, parseExpression(value));
            _heightDefined = true;
        }
        else if (name == "stroke") {
            bind(&_stroke, parseStringExpression(value), &MarkersSymbolizer::convertColor);
        }
        else if (name == "stroke-opacity") {
            bind(&_strokeOpacity, parseExpression(value));
        }
        else if (name == "stroke-width") {
            bind(&_strokeWidth, parseExpression(value));
        }
        else if (name == "spacing") {
            bind(&_spacing, parseExpression(value));
        }
        else if (name == "allow-overlap") {
            bind(&_allowOverlap, parseExpression(value));
        }
        else if (name == "ignore-placement") {
            bind(&_ignorePlacement, parseExpression(value));
        }
        else if (name == "transform") {
            _transformExpression = parseStringExpression(value);
            bind(&_transform, _transformExpression, &MarkersSymbolizer::convertTransform);
        }
        else if (name == "comp-op") {
            bind(&_compOp, parseStringExpression(value));
        }
        else if (name == "opacity") { // binds to 2 parameters
            bind(&_fillOpacity, parseExpression(value));
            bind(&_strokeOpacity, parseExpression(value));
        }
        else {
            Symbolizer::bindParameter(name, value);
        }
    }
    
    bool MarkersSymbolizer::containsRotationTransform(const Value& val) {
        try {
            std::vector<std::shared_ptr<Transform>> transforms = parseTransformList(boost::lexical_cast<std::string>(val));
            for (const std::shared_ptr<Transform>& transform : transforms) {
                if (std::dynamic_pointer_cast<RotateTransform>(transform)) {
                    return true;
                }
            }
            return false;
        }
        catch (const ParserException&) {
            return false;
        }
    }

    std::shared_ptr<vt::Bitmap> MarkersSymbolizer::makeEllipseBitmap(float width, float height, const vt::Color& color, float strokeWidth, const vt::Color& strokeColor) {
        int canvasWidth = static_cast<int>(std::ceil(width + strokeWidth));
        int canvasHeight = static_cast<int>(std::ceil(height + strokeWidth));
        vt::BitmapCanvas canvas(canvasWidth, canvasHeight);
        float x0 = canvasWidth * 0.5f, y0 = canvasHeight * 0.5f;
        if (strokeWidth > 0) {
            canvas.setColor(strokeColor);
            canvas.drawEllipse(x0, y0, (width + strokeWidth * 0.5f) * 0.5f, (height + strokeWidth * 0.5f) * 0.5f);
        }
        canvas.setColor(color);
        canvas.drawEllipse(x0, y0, (width - strokeWidth * 0.5f) * 0.5f, (height - strokeWidth * 0.5f) * 0.5f);
        return canvas.buildBitmap();
    }

    std::shared_ptr<vt::Bitmap> MarkersSymbolizer::makeArrowBitmap(float width, float height, const vt::Color& color, float strokeWidth, const vt::Color& strokeColor) {
        int canvasWidth = static_cast<int>(std::ceil(width + strokeWidth));
        int canvasHeight = static_cast<int>(std::ceil(height + strokeWidth));
        float x0 = strokeWidth * 0.5f, x1 = std::ceil(width - height * 0.5f), y1 = height * 1 / 3, y2 = height * 2 / 3;
        vt::BitmapCanvas canvas(canvasWidth, canvasHeight);
        if (strokeWidth > 0) {
            canvas.setColor(strokeColor);
            canvas.drawRectangle(0, y1 - strokeWidth * 0.5f, x1, y2 + strokeWidth * 0.5f);
            canvas.drawTriangle(x1 - strokeWidth * 0.5f, 0, x1 - strokeWidth * 0.5f, height, width, height * 0.5f);
        }
        canvas.setColor(color);
        canvas.drawRectangle(x0, y1, x1, y2);
        canvas.drawTriangle(x1, strokeWidth, x1, height - strokeWidth * 0.5f, width - strokeWidth * 0.5f, height * 0.5f);
        return canvas.buildBitmap();
    }
} }
