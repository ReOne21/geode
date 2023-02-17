#include <cocos2d.h>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/ranges.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/CCMenuItemToggler.hpp>

USE_GEODE_NAMESPACE();

#pragma warning(disable: 4273)

void CCNode::swapChildIndices(CCNode* first, CCNode* second) {
    m_pChildren->exchangeObject(first, second);
    std::swap(first->m_nZOrder, second->m_nZOrder);
    std::swap(first->m_uOrderOfArrival, second->m_uOrderOfArrival);
}

void CCNode::insertBefore(CCNode* child, CCNode* before) {
    this->addChild(child);
    if (m_pChildren->containsObject(before)) {
        child->setZOrder(before->getZOrder());
        child->setOrderOfArrival(before->getOrderOfArrival() - 1);
    }
}

void CCNode::insertAfter(CCNode* child, CCNode* after) {
    this->addChild(child);
    if (m_pChildren->containsObject(after)) {
        child->setZOrder(after->getZOrder());
        child->setOrderOfArrival(after->getOrderOfArrival() + 1);
    }
}

CCArray* Layout::getNodesToPosition(CCNode* on) {
    if (!on->getChildren()) {
        return CCArray::create();
    }
    return on->getChildren()->shallowCopy();
}

static AxisLayoutOptions const* axisOpts(CCNode* node) {
    if (!node) return nullptr;
    return typeinfo_cast<AxisLayoutOptions*>(node->getLayoutOptions());
}

static bool isOptsBreakLine(AxisLayoutOptions const* opts) {
    if (opts) {
        return opts->getBreakLine();
    }
    return false;
}

static bool isOptsSameLine(AxisLayoutOptions const* opts) {
    if (opts) {
        return opts->getSameLine();
    }
    return false;
}

static int optsScalePrio(AxisLayoutOptions const* opts) {
    if (opts) {
        return opts->getScalePriority();
    }
    return AXISLAYOUT_DEFAULT_PRIORITY;
}

static float optsMinScale(AxisLayoutOptions const* opts) {
    if (opts) {
        return opts->getMinScale();
    }
    return AXISLAYOUT_DEFAULT_MIN_SCALE;
}

static float optsMaxScale(AxisLayoutOptions const* opts) {
    if (opts) {
        return opts->getMaxScale();
    }
    return 1.f;
}

static float optsRelScale(AxisLayoutOptions const* opts) {
    if (opts) {
        return opts->getRelativeScale();
    }
    return 1.f;
}

static float scaleByOpts(AxisLayoutOptions const* opts, float scale, int prio) {
    if (prio > optsScalePrio(opts)) {
        return optsMaxScale(opts) * optsRelScale(opts);
    }
    // otherwise if it matches scale it down by the factor
    else if (prio == optsScalePrio(opts)) {
        auto trueScale = scale;
        auto min = optsMinScale(opts);
        auto max = optsMaxScale(opts);
        if (trueScale < min) {
            trueScale = min;
        }
        if (trueScale > max) {
            trueScale = max;
        }
        return trueScale * optsRelScale(opts);
    }
    // otherwise it's been scaled down to minimum
    else {
        return optsMinScale(opts) * optsRelScale(opts);
    }
}

struct AxisLayout::Row : public CCObject {
    float nextOverflowScaleDownFactor;
    float nextOverflowSquishFactor;
    float axisLength;
    float crossLength;
    float axisEndsLength;
    // all layout calculations happen within a single frame so no Ref needed
    CCArray* nodes;

    Row(
        float scaleFactor,
        float squishFactor,
        float axisLength,
        float crossLength,
        float axisEndsLength,
        CCArray* nodes
    ) : nextOverflowScaleDownFactor(scaleFactor),
        nextOverflowSquishFactor(squishFactor),
        axisLength(axisLength),
        crossLength(crossLength),
        axisEndsLength(axisEndsLength),
        nodes(nodes)
    {
        this->autorelease();
    }
};

struct AxisPosition {
    float axisLength;
    float axisAnchor;
    float crossLength;
    float crossAnchor;
};

static AxisPosition nodeAxis(CCNode* node, Axis axis, float scale) {
    auto scaledSize = node->getScaledContentSize() * scale;
    std::optional<float> axisLength = std::nullopt;
    if (auto opts = axisOpts(node)) {
        axisLength = opts->getLength();
    }
    // CCMenuItemToggler is a common quirky class
    if (auto toggle = typeinfo_cast<CCMenuItemToggler*>(node)) {
        scaledSize = toggle->m_offButton->getScaledContentSize();
    }
    auto anchor = node->getAnchorPoint();
    if (axis == Axis::Row) {
        return AxisPosition {
            .axisLength = axisLength.value_or(scaledSize.width),
            .axisAnchor = anchor.x,
            .crossLength = scaledSize.height,
            .crossAnchor = anchor.y,
        };
    }
    else {
        return AxisPosition {
            .axisLength = axisLength.value_or(scaledSize.height),
            .axisAnchor = anchor.y,
            .crossLength = scaledSize.width,
            .crossAnchor = anchor.x,
        };
    }
}

float AxisLayout::nextGap(AxisLayoutOptions const* now, AxisLayoutOptions const* next) const {
    std::optional<float> gap;
    if (now) {
        gap = now->getNextGap();
    }
    if (next && (!gap || gap.value() < next->getPrevGap())) {
        gap = next->getPrevGap();
    }
    return gap.value_or(m_gap);
}

bool AxisLayout::shouldAutoScale(AxisLayoutOptions const* opts) const {
    if (!opts) return m_autoScale;
    return opts->getAutoScale().value_or(m_autoScale);
}

float AxisLayout::minScaleForPrio(CCArray* nodes, int prio) const {
    float min = AXISLAYOUT_DEFAULT_MIN_SCALE;
    bool first = true;
    for (auto node : CCArrayExt<CCNode>(nodes)) {
        auto scale = optsMinScale(axisOpts(node));
        if (first) {
            min = scale;
            first = false;
        }
        else if (scale < min) {
            min = scale;
        }
    }
    return min;
}

float AxisLayout::maxScaleForPrio(CCArray* nodes, int prio) const {
    float max = 1.f;
    bool first = true;
    for (auto node : CCArrayExt<CCNode>(nodes)) {
        auto scale = optsMaxScale(axisOpts(node));
        if (first) {
            max = scale;
            first = false;
        }
        else if (scale > max) {
            max = scale;
        }
    }
    return max;
}

AxisLayout::Row* AxisLayout::fitInRow(
    CCNode* on, CCArray* nodes,
    std::pair<int, int> const& minMaxPrios,
    bool doAutoScale,
    float scale, float squish, int prio,
    bool finalPosCalc
) const {
    float nextAxisLength = 0.f;
    float axisLength = 0.f;
    float crossLength = 0.f;
    auto res = CCArray::create();

    auto available = nodeAxis(on, m_axis, 1.f);
    AxisLayoutOptions const* prev = nullptr;
    size_t ix = 0;
    for (auto& node : CCArrayExt<CCNode*>(nodes)) {
        auto opts = axisOpts(node);
        if (this->shouldAutoScale(opts)) {
            node->setScale(optsMaxScale(opts) * optsRelScale(opts));
        }
        auto nodeScale = scaleByOpts(opts, scale, prio);
        auto pos = nodeAxis(node, m_axis, nodeScale * squish);
        nextAxisLength += pos.axisLength;
        // if multiple rows are allowed and this row is full, time for the 
        // next row
        // also force at least one object to be added to this row, because if 
        // it's too large for this row it's gonna be too large for all rows
        if (
            !finalPosCalc && m_growCrossAxis && ((
                (nextAxisLength > available.axisLength) && 
                ix != 0 &&
                !isOptsSameLine(opts)
            ) || isOptsBreakLine(prev))
        ) {
            break;
        }
        res->addObject(node);
        if (ix) {
            auto gap = nextGap(prev, opts);
            // if we've exhausted all priority scale options, scale gap too
            if (prio == minMaxPrios.first) {
                nextAxisLength += gap * scale * squish;
                axisLength += gap * scale * squish;
            }
            else {
                nextAxisLength += gap * squish;
                axisLength += gap * squish;
            }
        }
        axisLength += pos.axisLength;
        if (pos.crossLength > crossLength) {
            crossLength = pos.crossLength;
        }
        prev = opts;
        ix++;
    }

    float axisEndsLength = 0.f;
    if (!finalPosCalc) {
        // whoops! removing objects from a CCArray while iterating is totes potes UB
        for (int i = 0; i < res->count(); i++) {
            nodes->removeFirstObject();
        }

        // reverse row if needed
        if (m_axisReverse) {
            res->reverseObjects();
        }
    
        if (res->count()) {
            auto first = static_cast<CCNode*>(res->firstObject());
            auto last = static_cast<CCNode*>(res->lastObject());
            axisEndsLength = (
                first->getScaledContentSize().width * scale / 2 +
                last->getScaledContentSize().width * scale / 2
            );
        }
    }

    return new Row(
        // how much should the nodes be scaled down to fit the next row
        available.axisLength / nextAxisLength * scale * squish,
        // how much should the nodes be squished to fit the next item in this 
        // row
        available.axisLength / nextAxisLength * scale * squish,
        axisLength,
        crossLength,
        axisEndsLength,
        res
    );
}

void AxisLayout::tryFitLayout(
    CCNode* on, CCArray* nodes,
    std::pair<int, int> const& minMaxPrios,
    bool doAutoScale,
    float scale, float squish, int prio
) const {
    // where do all of these magical calculations come from?
    // idk i got tired of doing the math but they work so ¯\_(ツ)_/¯ 
    // like i genuinely have no clue fr why some of these work tho, 
    // i just threw in random equations and numbers until it worked

    auto rows = CCArray::create();
    float totalRowCrossLength = 0.f;
    float crossScaleDownFactor = 0.f;
    float crossSquishFactor = 0.f;

    // fit everything into rows while possible
    size_t ix = 0;
    auto newNodes = nodes->shallowCopy();
    while (newNodes->count()) {
        auto row = this->fitInRow(
            on, newNodes,
            minMaxPrios, doAutoScale,
            scale, squish, prio,
            false
        );
        rows->addObject(row);
        if (
            row->nextOverflowScaleDownFactor > crossScaleDownFactor &&
            row->nextOverflowScaleDownFactor < scale
        ) {
            crossScaleDownFactor = row->nextOverflowScaleDownFactor;
        }
        if (
            row->nextOverflowSquishFactor > crossSquishFactor &&
            row->nextOverflowSquishFactor < squish
        ) {
            crossSquishFactor = row->nextOverflowSquishFactor;
        }
        totalRowCrossLength += row->crossLength;
        if (ix) {
            totalRowCrossLength += m_gap;
        }
        ix++;
    }
    newNodes->release();

    auto available = nodeAxis(on, m_axis, 1.f);

    // if cross axis overflow not allowed and it's overflowing, try to scale 
    // down layout if there are any nodes with auto-scale enabled (or 
    // auto-scale is enabled by default)
    if (
        !m_allowCrossAxisOverflow && 
        doAutoScale && 
        totalRowCrossLength > available.crossLength
    ) {
        bool attemptRescale = false;
        auto minScaleForPrio = this->minScaleForPrio(nodes, prio);
        if (
            // if the scale is less than the lowest min scale allowed, then 
            // trying to scale will have no effect and not help anywmore
            crossScaleDownFactor < minScaleForPrio ||
            // if the scale down factor is the same as before, then we've 
            // entered an infinite loop
            crossScaleDownFactor == scale
        ) {
            // is there still some lower priority nodes we could try scaling?
            if (prio > minMaxPrios.first) {
                while (true) {
                    prio -= 1;
                    auto scale = this->maxScaleForPrio(nodes, prio);
                    if (!scale) {
                        continue;
                    }
                    crossScaleDownFactor = scale;
                    break;
                }
                attemptRescale = true;
            }
            // otherwise we're just gonna squish
        }
        // otherwise scale as usual
        else {
            attemptRescale = true;
        }
        if (attemptRescale) {
            rows->release();
            return this->tryFitLayout(
                on, nodes,
                minMaxPrios, doAutoScale,
                crossScaleDownFactor, squish, prio
            );
        }
    }

    // if we're still overflowing, squeeze nodes closer together
    if (!m_allowCrossAxisOverflow && totalRowCrossLength > available.crossLength) {
        // if squishing rows would take less squishing that squishing columns, 
        // then squish rows
        if (totalRowCrossLength / available.crossLength < crossSquishFactor) {
            rows->release();
            return this->tryFitLayout(
                on, nodes,
                minMaxPrios, doAutoScale,
                scale, crossSquishFactor, prio
            );
        }
    }

    // if we're here, the nodes are ready to be positioned

    if (m_crossReverse) {
        rows->reverseObjects();
    }

    // resize cross axis if needed
    if (m_allowCrossAxisOverflow) {
        available.crossLength = totalRowCrossLength;
        if (m_axis == Axis::Row) {
            on->setContentSize({
                available.axisLength,
                totalRowCrossLength,
            });
        }
        else {
            on->setContentSize({
                totalRowCrossLength,
                available.axisLength,
            });
        }
    }

    float columnSquish = 1.f;
    if (!m_allowCrossAxisOverflow && totalRowCrossLength > available.crossLength) {
        columnSquish = available.crossLength / totalRowCrossLength;
        totalRowCrossLength *= columnSquish;
    }

    float rowsEndsLength = 0.f;
    if (rows->count()) {
        auto first = static_cast<Row*>(rows->firstObject());
        auto last = static_cast<Row*>(rows->lastObject());
        rowsEndsLength = first->crossLength / 2 + last->crossLength / 2;
    }

    float rowCrossPos;
    switch (m_crossAlignment) {
        case AxisAlignment::Start: {
            rowCrossPos = totalRowCrossLength - rowsEndsLength * 1.5f * scale * (1.f - columnSquish);
        } break;

        case AxisAlignment::Even: {
            totalRowCrossLength = available.crossLength;
            rowCrossPos = totalRowCrossLength - rowsEndsLength * 1.5f * scale * (1.f - columnSquish);
        } break;

        case AxisAlignment::Center: {
            rowCrossPos = available.crossLength / 2 + totalRowCrossLength / 2 - 
                rowsEndsLength * 1.5f * scale * (1.f - columnSquish);
        } break;

        case AxisAlignment::End: {
            rowCrossPos = available.crossLength - 
                rowsEndsLength * 1.5f * scale * (1.f - columnSquish);
        } break;
    }

    float rowEvenSpace = available.crossLength / rows->count();

    for (auto row : CCArrayExt<Row*>(rows)) {
        if (m_crossAlignment == AxisAlignment::Even) {
            rowCrossPos -= rowEvenSpace / 2 + row->crossLength / 2;
        }
        else {
            rowCrossPos -= row->crossLength * columnSquish;
        }

        // scale down & squish row if it overflows main axis
        float rowScale = scale;
        float rowSquish = squish;
        if (row->axisLength > available.axisLength) {
            row->axisLength /= scale * squish;
            if (m_autoScale) {
                rowScale = available.axisLength / row->axisLength;
                if (rowScale < AXISLAYOUT_DEFAULT_MIN_SCALE) {
                    rowScale = AXISLAYOUT_DEFAULT_MIN_SCALE;
                }
                row->axisLength *= rowScale;
            }
            // squishing needs to take into account the row ends
            if (row->axisLength > available.axisLength) {
                rowSquish = available.axisLength / row->axisLength;
            }
            row->axisLength *= rowSquish;
        }

        float rowAxisPos;
        switch (m_axisAlignment) {
            case AxisAlignment::Start: { 
                rowAxisPos = 0.f;
            } break;

            case AxisAlignment::Even: { 
                rowAxisPos = 0.f;
            } break;

            case AxisAlignment::Center: {
                rowAxisPos = available.axisLength / 2 - row->axisLength / 2;
            } break;

            case AxisAlignment::End: {
                rowAxisPos = available.axisLength - row->axisLength;
            } break;
        }

        float evenSpace = available.axisLength / row->nodes->count();

        size_t ix = 0;
        AxisLayoutOptions const* prev = nullptr;
        for (auto& node : CCArrayExt<CCNode*>(row->nodes)) {
            auto scale = rowScale;
            auto opts = axisOpts(node);
            // rescale node if overflowing
            if (this->shouldAutoScale(opts)) {
                scale = scaleByOpts(opts, scale, prio);
                // CCMenuItemSpriteExtra is quirky af
                if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(node)) {
                    btn->m_baseScale = scale;
                }
                node->setScale(scale);
            }
            if (!ix) {
                rowAxisPos += row->axisEndsLength * scale / 2 * (1.f - rowSquish);
            }
            auto pos = nodeAxis(node, m_axis, rowSquish);
            float axisPos;
            if (m_axisAlignment == AxisAlignment::Even) {
                axisPos = rowAxisPos + evenSpace / 2 - pos.axisLength * (.5f - pos.axisAnchor);
                rowAxisPos += evenSpace - 
                    row->axisEndsLength * scale * (1.f - rowSquish) * 1.f / nodes->count();
            }
            else {
                if (ix) {
                    rowAxisPos += this->nextGap(prev, opts) * scale * rowSquish;
                }
                axisPos = rowAxisPos + pos.axisLength * pos.axisAnchor;
                rowAxisPos += pos.axisLength - 
                    row->axisEndsLength * scale * (1.f - rowSquish) * 1.f / nodes->count();
            }
            float crossOffset;
            switch (m_crossAlignment) {
                case AxisAlignment::Start: {
                    crossOffset = pos.crossLength * pos.crossAnchor;
                } break;

                case AxisAlignment::Center: case AxisAlignment::Even: {
                    crossOffset = row->crossLength / 2 - pos.crossLength * (.5f - pos.crossAnchor);
                } break;

                case AxisAlignment::End: {
                    crossOffset = row->crossLength - pos.crossLength * (1.f - pos.crossAnchor);
                } break;
            }
            if (m_axis == Axis::Row) {
                node->setPosition(axisPos, rowCrossPos + crossOffset);
            }
            else {
                node->setPosition(rowCrossPos + crossOffset, axisPos);
            }
            prev = opts;
            ix++;
        }
    
        if (m_crossAlignment == AxisAlignment::Even) {
            rowCrossPos -= rowEvenSpace / 2 - row->crossLength / 2 - 
                rowsEndsLength * 1.5f * scale * (1.f - columnSquish) * 1.f / rows->count();
        }
        else {
            rowCrossPos -= m_gap * columnSquish - 
                rowsEndsLength * 1.5f * scale * (1.f - columnSquish) * 1.f / rows->count();
        }
    }
}

void AxisLayout::apply(CCNode* on) {
    auto nodes = getNodesToPosition(on);
    
    std::pair<int, int> minMaxPrio;
    bool doAutoScale = false;

    bool first = true;
    for (auto node : CCArrayExt<CCNode>(nodes)) {
        int prio = 0;
        if (auto opts = axisOpts(node)) {
            prio = opts->getScalePriority();
            // this does cause a recheck of m_autoScale every iteration but it 
            // should be pretty fast and this correctly handles the situation 
            // where auto-scale is enabled on the layout but explicitly 
            // disabled on all its children
            if (opts->getAutoScale().value_or(m_autoScale)) {
                doAutoScale = true;
            }
        }
        else {
            if (m_autoScale) {
                doAutoScale = true;
            }
        }
        if (first) {
            minMaxPrio = { prio, prio };
            first = false;
        }
        else {
            if (prio < minMaxPrio.first) {
                minMaxPrio.first = prio;
            }
            if (prio > minMaxPrio.second) {
                minMaxPrio.second = prio;
            }
        }
    }

    this->tryFitLayout(
        on, nodes,
        minMaxPrio, doAutoScale,
        this->maxScaleForPrio(nodes, minMaxPrio.second), 1.f, minMaxPrio.second
    );
}

AxisLayout::AxisLayout(Axis axis) : m_axis(axis) {}

Axis AxisLayout::getAxis() const {
    return m_axis;
}

AxisAlignment AxisLayout::getCrossAxisAlignment() const {
    return m_crossAlignment;
}

AxisAlignment AxisLayout::getAxisAlignment() const {
    return m_axisAlignment;
}

float AxisLayout::getGap() const {
    return m_gap;
}

bool AxisLayout::getAxisReverse() const {
    return m_axisReverse;
}

bool AxisLayout::getCrossAxisReverse() const {
    return m_crossReverse;
}

bool AxisLayout::getAutoScale() const {
    return m_autoScale;
}

bool AxisLayout::getGrowCrossAxis() const {
    return m_growCrossAxis;
}

bool AxisLayout::getCrossAxisOverflow() const {
    return m_allowCrossAxisOverflow;
}

AxisLayout* AxisLayout::setAxis(Axis axis) {
    m_axis = axis;
    return this;
}

AxisLayout* AxisLayout::setCrossAxisAlignment(AxisAlignment align) {
    m_crossAlignment = align;
    return this;
}

AxisLayout* AxisLayout::setAxisAlignment(AxisAlignment align) {
    m_axisAlignment = align;
    return this;
}

AxisLayout* AxisLayout::setGap(float gap) {
    m_gap = gap;
    return this;
}

AxisLayout* AxisLayout::setAxisReverse(bool reverse) {
    m_axisReverse = reverse;
    return this;
}

AxisLayout* AxisLayout::setCrossAxisReverse(bool reverse) {
    m_crossReverse = reverse;
    return this;
}

AxisLayout* AxisLayout::setCrossAxisOverflow(bool fit) {
    m_allowCrossAxisOverflow = fit;
    return this;
}

AxisLayout* AxisLayout::setAutoScale(bool scale) {
    m_autoScale = scale;
    return this;
}

AxisLayout* AxisLayout::setGrowCrossAxis(bool shrink) {
    m_growCrossAxis = shrink;
    return this;
}

AxisLayout* AxisLayout::create(Axis axis) {
    return new AxisLayout(axis);
}

// RowLayout

RowLayout::RowLayout() : AxisLayout(Axis::Row) {}

RowLayout* RowLayout::create() {
    return new RowLayout();
}

// ColumnLayout

ColumnLayout::ColumnLayout() : AxisLayout(Axis::Column) {}

ColumnLayout* ColumnLayout::create() {
    return new ColumnLayout();
}

// AxisLayoutOptions

AxisLayoutOptions* AxisLayoutOptions::create() {
    return new AxisLayoutOptions();
}

std::optional<bool> AxisLayoutOptions::getAutoScale() const {
    return m_autoScale;
}

float AxisLayoutOptions::getMaxScale() const {
    return m_maxScale;
}

float AxisLayoutOptions::getMinScale() const {
    return m_minScale;
}

float AxisLayoutOptions::getRelativeScale() const {
    return m_relativeScale;
}

std::optional<float> AxisLayoutOptions::getLength() const {
    return m_length;
}

std::optional<float> AxisLayoutOptions::getPrevGap() const {
    return m_prevGap;
}

std::optional<float> AxisLayoutOptions::getNextGap() const {
    return m_nextGap;
}

bool AxisLayoutOptions::getBreakLine() const {
    return m_breakLine;
}

bool AxisLayoutOptions::getSameLine() const {
    return m_sameLine;
}

int AxisLayoutOptions::getScalePriority() const {
    return m_scalePriority;
}

AxisLayoutOptions* AxisLayoutOptions::setMaxScale(float scale) {
    m_maxScale = scale;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setMinScale(float scale) {
    m_minScale = scale;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setRelativeScale(float scale) {
    m_relativeScale = scale;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setAutoScale(std::optional<bool> enabled) {
    m_autoScale = enabled;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setLength(std::optional<float> length) {
    m_length = length;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setPrevGap(std::optional<float> gap) {
    m_prevGap = gap;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setNextGap(std::optional<float> gap) {
    m_nextGap = gap;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setBreakLine(bool enable) {
    m_breakLine = enable;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setSameLine(bool enable) {
    m_sameLine = enable;
    return this;
}

AxisLayoutOptions* AxisLayoutOptions::setScalePriority(int priority) {
    m_scalePriority = priority;
    return this;
}
