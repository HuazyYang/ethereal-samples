// SPDX-License-Identifier: Apache-2.0
// Independent CPU semantic tests. The retained implementation is read as an
// oracle; this fixture does not link GPDevice, invoke a GPU, or duplicate its recursion.
#include "../GVDBHostVolume.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
namespace g = gvdb::portable;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void check(nvrhi::FRESULT status, const char* message) { require(NVRHI_SUCCEEDED(status), message); }
bool same(g::Int3 a, g::Int3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
g::LevelConfig shape(std::initializer_list<uint32_t> dimensions) {
    g::LevelConfig config;
    for (uint32_t value : dimensions) {
        config.logDimension[config.levelCount] = value;
        config.initialNodeCapacity[config.levelCount++] = 1;
    }
    return config;
}
g::NodeId id(uint32_t level, uint64_t index) {
    g::NodeId result;
    require(g::NodeId::encode(0, level, index, result), "fixture ID");
    return result;
}
g::NodeId activate(g::HostVolume& volume, double x, double y = .5, double z = .5) {
    g::NodeId result;
    const auto status = volume.activate(x, y, z, result);
    if (NVRHI_FAILED(status)) std::fprintf(stderr, "activate(%g,%g,%g) failed: 0x%08x, root level %u\n",
        x, y, z, unsigned(status), volume.root().level());
    check(status, "activate expected point");
    require(!result.isNull(), "successful activation has output");
    return result;
}
std::vector<uint8_t> records(const g::HostVolume& volume) {
    std::vector<uint8_t> result;
    for (unsigned group = 0; group < 2; ++group) for (unsigned level = 0; level < volume.configuration().levelCount; ++level) {
        const auto pool = volume.pool(group, level);
        const size_t begin = result.size(), size = size_t(pool.count) * pool.stride;
        result.resize(begin + size);
        if (size) std::memcpy(result.data() + begin, pool.data, size);
    }
    return result;
}
void legacyPositive() {
    g::HostVolume volume;
    check(volume.configure(shape({2, 1, 1})), "configure positive reference");
    require(activate(volume, 0) == id(0, 0) && volume.root() == id(0, 0), "first brick is root");
    require(activate(volume, 3.999) == id(0, 0), "same brick idempotent");
    require(activate(volume, 4) == id(0, 1) && volume.root() == id(1, 0), "first root growth IDs");
    require(activate(volume, 8) == id(0, 2) && volume.root() == id(2, 0), "second root growth IDs");
    require(activate(volume, 15) == id(0, 3), "last brick in root");
    require(volume.count(0, 0) == 4 && volume.count(0, 1) == 2 && volume.count(0, 2) == 1,
            "legacy positive pool allocation counts");
    for (unsigned i = 0; i < 4; ++i)
        require(same(volume.node(id(0, i))->position, {int32_t(i * 4), 0, 0}), "legacy positive leaf positions");
    require(same(volume.node(id(1, 0))->position, {0, 0, 0}) &&
            same(volume.node(id(1, 1))->position, {8, 0, 0}) &&
            same(volume.node(volume.root())->position, {0, 0, 0}), "legacy positive parent origins");
    const auto* rootChildren = volume.childList(volume.node(volume.root())->children);
    require(rootChildren[0] == id(1, 0) && rootChildren[1] == id(1, 1), "legacy x-fast child links");
    check(volume.finishTopology(), "finish positive topology");
    require(volume.activeLeafCount() == 4 && same(volume.bounds().lower, {0, 0, 0}) &&
            same(volume.bounds().upper, {16, 4, 4}), "positive half-open bounds");
    const auto before = records(volume); const auto root = volume.root();
    const auto revision = volume.revision().topology;
    g::NodeId missing = id(0, 0);
    require(volume.activate(16, .5, .5, missing) == nvrhi::FE_UNSUPPORTED && missing.isNull(),
            "configured maximum root rejects further growth");
    require(records(volume) == before && volume.root() == root && volume.revision().topology == revision && !volume.dirty(),
            "extent failure is transactional");
    check(volume.reserve(0, 0, 50), "reserve node pool");
    require(volume.capacity(0, 0) >= 50 && volume.count(0, 0) == 4 && records(volume) == before,
            "reservation growth preserves used records and IDs");
    g::PackedHostTopology packed;
    check(volume.packTopology(packed), "pack finished topology");
    require(packed.volume.hierarchy.x == 2 && packed.volume.hierarchy.y == 0 && packed.levels.size() == 3 &&
            packed.nodes.size() == 7 * 16 && packed.children.size() == 3 * 8 * 2,
            "packed offsets/counts independently expected");
    require(packed.levels[1].nodes.x == 4 * 64 && packed.levels[2].nodes.x == 6 * 64 &&
            packed.levels[1].children.x == 0 && packed.levels[2].children.x == 2 * 8 * 8,
            "packed level byte offsets");
}
void legacyNegative() {
    g::HostVolume volume;
    check(volume.configure(shape({2, 1, 1})), "configure negative reference");
    require(activate(volume, -1) == id(0, 0) && activate(volume, -4) == id(0, 0), "negative floor same brick");
    require(activate(volume, -5) == id(0, 1) && volume.root() == id(1, 0), "negative first growth");
    require(activate(volume, -9) == id(0, 2) && volume.root() == id(2, 0), "negative second growth");
    require(activate(volume, -16) == id(0, 3), "negative edge brick");
    const int32_t expected[] = {-4, -8, -12, -16};
    for (unsigned i = 0; i < 4; ++i)
        require(volume.node(id(0, i))->position.x == expected[i], "negative allocation order");
    require(volume.node(id(1, 0))->position.x == -8 && volume.node(id(1, 1))->position.x == -16 &&
            volume.node(volume.root())->position.x == -16, "negative canonical parent origins");
    const auto* children = volume.childList(volume.node(volume.root())->children);
    require(children[0] == id(1, 1) && children[1] == id(1, 0), "negative root preserves old parent ID");
    check(volume.finishTopology(), "finish negative reference");
    require(same(volume.bounds().lower, {-16, 0, 0}) && same(volume.bounds().upper, {0, 4, 4}), "negative bounds");
}
void signedGrowthAndPromotion() {
    g::HostVolume volume;
    check(volume.configure(shape({2, 1, 1, 1})), "configure signed extension");
    const auto original = activate(volume, 0, 0, 0);
    const auto negative = activate(volume, -1, -1, -1);
    require(same(volume.node(volume.root())->position, {-4, -4, -4}) && volume.root().level() == 1,
            "cross-zero parent contains both whole bricks");
    const auto shifted = activate(volume, -5, 3, 3);
    require(volume.root().level() == 2 && same(volume.node(volume.root())->position, {-12, -4, -4}),
            "shifted root preserves prior subtree extent and alignment");
    const auto farther = activate(volume, 9, 3, 3);
    require(volume.root().level() == 3 && same(volume.node(volume.root())->position, {-12, -4, -4}),
            "shifted-root subsequent growth preserves whole prior root");
    require(volume.findLeaf({0, 0, 0}) == original && volume.findLeaf({-1, -1, -1}) == negative &&
            volume.findLeaf({-5, 3, 3}) == shifted && volume.findLeaf({9, 3, 3}) == farther,
            "signed growth retains every prior leaf ID");
    check(volume.finishTopology(), "finish signed topology");
    g::HostVolume promotion;
    check(promotion.configure(shape({2, 1, 1})), "configure promotion");
    const auto leaf = activate(promotion, 1);
    g::NodeId promoted;
    check(promotion.activateAtLevel(2, {1, 0, 0}, promoted), "promote containing leaf to level2");
    require(promoted == promotion.root() && promoted.level() == 2 && promotion.findLeaf({1, 0, 0}) == leaf,
            "same-position promotion keeps original leaf");
    require(promotion.count(0, 0) == 1 && promotion.count(0, 1) == 1 && promotion.count(0, 2) == 1,
            "promotion creates ancestors without extra leaves");
    check(promotion.finishTopology(), "finish promoted topology");
}
void batchRootEnclosure() {
    const g::Int3 centers[]={{8,12,8},{8,8,4}};
    const g::Int3 lower[]={{4,12,8},{8,8,0}},upper[]={{8,16,12},{12,12,4}};
    const g::IndexBounds bounds{{4,8,0},{16,20,16},false};
    // Independent FloatSupport radius1 around (8,14,10),(11,11,4).
    g::HostVolume sequential;check(sequential.configure(shape({2,1,1,1})),"configure sequential reference");
    for(auto c:centers)activate(sequential,c.x,c.y,c.z);
    bool rejected=false;
    for(unsigned p=0;p<2&&!rejected;++p)
        for(int x=lower[p].x;x<=upper[p].x&&!rejected;x+=4)
            for(int y=lower[p].y;y<=upper[p].y&&!rejected;y+=4)
                for(int z=lower[p].z;z<=upper[p].z&&!rejected;z+=4){g::NodeId unused;rejected=sequential.activateAtLevel(0,{x,y,z},unused)==nvrhi::FE_UNSUPPORTED;}
    require(rejected,"two-point sequential reference exhausts root levels");
    for(bool existing:{false,true}) {
        g::HostVolume volume;check(volume.configure(shape({2,1,1,1})),"configure batch enclosure");
        g::NodeId retained;
        if(existing){retained=activate(volume,8,12,8);check(volume.setActive(retained,false),"retain inactive leaf");check(volume.finishTopology(),"finish seed");}
        const auto epoch=volume.revision().epoch;
        check(volume.encloseBounds(bounds),"enclose whole support");
        require(volume.root().level()==2&&volume.revision().epoch==epoch,"smallest root and unchanged epoch");
        if(existing)require(volume.findLeaf({8,12,8})==retained&&!volume.isActive({8,12,8}),"existing ID and inactive flag retained");
        for(unsigned i=0;i<2;++i){g::NodeId leaf;check(volume.activateAtLevel(0,centers[i],leaf),"activate enclosed center");require(leaf==id(0,i),"center leaf allocation order");check(volume.setActive(leaf,true),"reactivate center");}
        for(unsigned p=0;p<2;++p)for(int x=lower[p].x;x<=upper[p].x;x+=4)for(int y=lower[p].y;y<=upper[p].y;y+=4)for(int z=lower[p].z;z<=upper[p].z;z+=4)activate(volume,x,y,z);
        check(volume.finishTopology(),"finish complete support");
        require(volume.activeLeafCount()==16&&same(volume.bounds().lower,bounds.lower)&&same(volume.bounds().upper,bounds.upper),"sixteen unique leaves and exact bounds");
        const auto before=records(volume);const auto root=volume.root();const auto revision=volume.revision().topology;
        check(volume.encloseBounds(bounds),"contained bounds no-op");require(records(volume)==before&&!volume.dirty(),"contained bounds preserve all records");
        require(volume.encloseBounds({{-100,-100,-100},{100,100,100},false})==nvrhi::FE_UNSUPPORTED,"true extent overflow rejects");
        require(records(volume)==before&&volume.root()==root&&!volume.dirty()&&volume.revision().topology==revision,"extent failure rollback");
    }
    g::HostVolume shifted;check(shifted.configure(shape({2,1,1,1})),"configure shifted enclosure");
    g::NodeId original;check(shifted.allocateNode(0,{1,-3,5},false,original),"allocate shifted root");check(shifted.setRoot(original),"set shifted root");check(shifted.finishTopology(),"finish shifted root");
    check(shifted.encloseBounds({{-3,-7,1},{9,5,13},false}),"enclose shifted phase");
    const auto pos=shifted.node(shifted.root())->position;
    require((pos.x-1)%4==0&&(pos.y+3)%4==0&&(pos.z-5)%4==0&&shifted.findLeaf({1,-3,5})==original&&!shifted.isActive({1,-3,5}),"phase ID flags preserved");check(shifted.finishTopology(),"validate shifted graph");
    g::HostVolume wrapped;check(wrapped.configure(shape({2,1,1,1,1})),"configure multi-level wrapper");
    const auto leaf0=activate(wrapped,0,0,0),leaf1=activate(wrapped,4,0,0);
    const auto priorRoot=wrapped.root();check(wrapped.setActive(priorRoot,false),"inactive prior root");
    check(wrapped.setActive(leaf1,false),"inactive prior child");check(wrapped.finishTopology(),"finish prior graph");
    const auto oldNode=*wrapped.node(priorRoot);const auto oldLeaf0=*wrapped.node(leaf0),oldLeaf1=*wrapped.node(leaf1);
    std::array<g::NodeId,8> oldLinks{};std::memcpy(oldLinks.data(),wrapped.childList(oldNode.children),sizeof(oldLinks));
    check(wrapped.encloseBounds({{-8,-8,-8},{16,16,16},false}),"wrap old subtree across two levels");
    require(wrapped.root().level()==3&&wrapped.node(priorRoot)->flags==oldNode.flags&&
        same(wrapped.node(priorRoot)->position,oldNode.position)&&wrapped.node(priorRoot)->children==oldNode.children&&
        wrapped.node(leaf0)->parent==oldLeaf0.parent&&wrapped.node(leaf1)->parent==oldLeaf1.parent&&
        wrapped.node(leaf0)->flags==oldLeaf0.flags&&wrapped.node(leaf1)->flags==oldLeaf1.flags&&
        std::memcmp(oldLinks.data(),wrapped.childList(oldNode.children),sizeof(oldLinks))==0,
        "multi-level wrapping retains old IDs origins flags and exact child links");
    check(wrapped.finishTopology(),"validate wrapped subtree");
    const auto wrappedBefore=records(wrapped);const auto wrappedRoot=wrapped.root();
    require(wrapped.encloseBounds({{1,0,0},{1,4,4},false})==nvrhi::FE_INVALID_ARGS&&
        records(wrapped)==wrappedBefore&&wrapped.root()==wrappedRoot&&!wrapped.dirty(),"invalid enclosure rollback");
    g::HostVolume maximum;check(maximum.configure(shape({2,1})),"configure maximum root");
    check(maximum.encloseBounds({{0,0,0},{8,8,8},false}),"allocate maximum root");
    check(maximum.finishTopology(),"finish maximum root");const auto maximumBefore=records(maximum);
    require(maximum.encloseBounds({{-4,0,0},{0,4,4},false})==nvrhi::FE_UNSUPPORTED&&records(maximum)==maximumBefore&&!maximum.dirty(),
        "published maximum root cannot be moved to accept external point");
    g::HostVolume empty;check(empty.configure(shape({2,1,1})),"configure empty enclosure");check(empty.encloseBounds({}),"empty bounds no-op");require(empty.root().isNull(),"no fabricated leaf");
}
void allocationRollbackAndClear() {
    g::HostVolume volume({64}); // One node and one eight-reference list per level.
    check(volume.configure(shape({1, 1, 1})), "configure bounded pools");
    const auto original = activate(volume, 0);
    check(volume.finishTopology(), "finish initial bounded tree");
    const auto before = records(volume); const auto oldEpoch = volume.revision().epoch;
    g::NodeId output;
    require(volume.activate(4, .5, .5, output) == nvrhi::FE_OUT_OF_MEMORY && output.isNull(),
            "budget exhausts after new root and old-root path were prepared");
    require(volume.root() == original && records(volume) == before && volume.count(0, 1) == 0 &&
            volume.count(0, 2) == 0 && volume.count(1, 1) == 0 && volume.count(1, 2) == 0 &&
            volume.node(original)->parent.isNull() && !volume.dirty(), "partial-growth rollback restores links/counts/root");
    require(NVRHI_FAILED(volume.reserve(0, 0, UINT64_MAX)) && records(volume) == before, "oversize reservation rejects safely");
    auto bad = shape({1, 1, 1}); bad.initialNodeCapacity[1] = UINT32_MAX;
    require(NVRHI_FAILED(volume.configure(bad)) && volume.root() == original && records(volume) == before,
            "failed reconfiguration preserves previous volume");
    const auto capacity = volume.capacity(0, 0);
    check(volume.clear(), "clear bounded volume");
    require(volume.root().isNull() && !volume.count(0, 0) && volume.capacity(0, 0) == capacity &&
            volume.revision().epoch == oldEpoch + 1 && volume.configuration().levelCount == 3,
            "clear retains reservations/config and changes identity epoch");
    g::PackedHostTopology packed;
    check(volume.packTopology(packed), "pack configured empty topology");
    require(packed.volume.hierarchy.y == UINT32_MAX && packed.levels.size() == 3 && packed.nodes.empty() &&
            packed.children.empty() && !packed.activeLeafCount && packed.volume.boundsMin.x == 0 &&
            packed.volume.boundsMax.x == 0 && packed.volume.atlasResolutionAndApron.x == 0,
            "empty export has no synthetic leaf, atlas, or stale bytes");
    const auto reused = activate(volume, -1);
    require(reused == original && volume.node(reused)->parent.isNull() && volume.node(reused)->children.isNull() &&
            same(volume.node(reused)->atlasInterior, {-1, -1, -1}), "reuse initializes records; epoch disambiguates ID reuse");
}
void manualValidationAndFlags() {
    g::HostVolume volume;
    check(volume.configure(shape({2, 1})), "configure manual tree");
    g::NodeId parent, child, list;
    check(volume.allocateNode(1, {0, 0, 0}, true, parent), "allocate parent");
    check(volume.allocateNode(0, {0, 0, 0}, true, child), "allocate child");
    check(volume.allocateChildList(1, list), "allocate list");
    for (unsigned i = 0; i < 8; ++i) require(volume.childList(list)[i].isNull(), "new child list null initialization");
    require(NVRHI_FAILED(volume.allocateChildList(0, list)) && list.isNull(), "leaf cannot allocate child list");
    require(g::NodeId::encode(1, 1, 0, list), "recover first list ID");
    volume.editNode(parent)->children = list; volume.editNode(child)->parent = parent;
    volume.editChildList(list)[0] = child;
    check(volume.setRoot(parent), "set valid manual root");
    g::PackedHostTopology untouched; untouched.activeLeafCount = 99;
    require(NVRHI_FAILED(volume.packTopology(untouched)) && untouched.activeLeafCount == 99,
            "uncommitted manual edits cannot be packed and output stays unchanged");
    check(volume.finishTopology(), "commit valid manual topology");
    const uint64_t revision = volume.revision().topology;
    volume.editNode(child)->position.x = 1;
    require(NVRHI_FAILED(volume.finishTopology()) && volume.revision().topology == revision,
            "child cell geometry validated before publication");
    volume.editNode(child)->position.x = 0;
    volume.editChildList(list)[1] = child;
    require(NVRHI_FAILED(volume.finishTopology()), "duplicate misplaced child rejected");
    volume.editChildList(list)[1] = {};
    volume.editNode(child)->parent = {};
    require(NVRHI_FAILED(volume.finishTopology()), "orphan/reciprocal parent mismatch rejected");
    volume.editNode(child)->parent = parent;
    check(volume.finishTopology(), "repair manual tree");
    check(volume.setActive(child, false), "disable leaf flag");
    require(volume.isAllocated({1, 1, 1}) && !volume.isActive({1, 1, 1}), "allocation is distinct from activity");
    require(activate(volume, 1, 1, 1) == child && !volume.isActive({1, 1, 1}), "activation preserves existing leaf flag like legacy");
    check(volume.finishTopology(), "finish inactive tree");
    require(!volume.activeLeafCount() && volume.bounds().empty && !volume.root().isNull(),
            "allocated all-inactive tree differs from cleared pools");
    check(volume.setActive(child, true), "explicitly reactivate leaf");
    check(volume.finishTopology(), "finish reactivated tree");
    require(volume.activeLeafCount() == 1 && same(volume.bounds().upper, {4, 4, 4}), "active flag restores bounds");
}
void mismatchedChildListValidation() {
    g::HostVolume volume;
    check(volume.configure(shape({1, 1, 2})), "configure different child-list strides");
    g::NodeId root;
    check(volume.activateAtLevel(2, {0, 0, 0}, root), "allocate explicit high root");
    const auto leaf = activate(volume, 12.5, 12.5, 12.5);
    check(volume.finishTopology(), "finish variable-stride topology");
    const auto originalList = volume.node(root)->children;
    const auto smallList = volume.node(volume.node(leaf)->parent)->children;
    volume.editNode(root)->children = smallList; // Root's slot63 must not index an eight-entry level1 list.
    require(NVRHI_FAILED(volume.validate()) && volume.findLeaf({12, 12, 12}).isNull(),
            "cross-level child-list references reject before indexing their smaller allocation");
    g::NodeId rejected;
    require(NVRHI_FAILED(volume.activate(12.5, 12.5, 12.5, rejected)) && rejected.isNull(),
            "activation validates pending malformed pool edits");
    volume.editNode(root)->children = originalList;
    check(volume.finishTopology(), "repair child-list reference");
    require(volume.findLeaf({12, 12, 12}) == leaf, "repair preserves original leaf identity");
}
void extentAndInputValidation() {
    g::HostVolume volume;
    check(volume.configure(shape({2, 1, 1})), "configure extents");
    g::CellExtents extents;
    check(volume.computeExtents(2, {-1, -1, -1, 0}, {1, 1, 1, 0}, extents), "sub-cover extents");
    require(same(extents.minimum, {0, 0, 0}) && same(extents.maximum, {0, 0, 0}) && extents.count == 1,
            "legacy extents truncate after float subtraction instead of flooring");
    check(volume.computeExtents(1, {0, 0, 0, 0}, {8, 4, 4, 0}, extents), "two-cell extents");
    require(same(extents.resolution, {2, 1, 1}) && extents.count == 2 && extents.cellWidth == 4,
            "extents are x-fast inclusive cell indices");
    g::NodeId output;
    require(NVRHI_FAILED(volume.activate(std::numeric_limits<double>::quiet_NaN(), 0, 0, output)) && output.isNull(), "NaN position rejected");
    require(NVRHI_FAILED(volume.activate(std::numeric_limits<double>::infinity(), 0, 0, output)), "infinite position rejected");
    require(NVRHI_FAILED(volume.activate(double(INT32_MAX) + 1, 0, 0, output)), "out-of-range float conversion rejected");
    require(NVRHI_FAILED(volume.activate(INT32_MAX, 0, 0, output)) && volume.root().isNull(), "unrepresentable exclusive upper bound rejected transactionally");
    const auto minimum = activate(volume, INT32_MIN, 0, 0);
    require(volume.node(minimum)->position.x == INT32_MIN, "minimum signed aligned origin accepted");
    check(volume.finishTopology(), "finish signed limit leaf");
}
g::CellExtents extent(g::HostVolume& volume, uint32_t level, g::Float4 minimum, g::Float4 maximum) {
    g::CellExtents result;
    check(volume.computeExtents(level, minimum, maximum, result), "fixture extents");
    return result;
}
void regionOrderingAndThresholds() {
    g::HostVolume volume;
    check(volume.configure(shape({1,1,1,1})), "configure region ordering");
    const auto region = extent(volume, 1, {0,0,0,0}, {4,6,4,0});
    uint64_t selected = 99;
    check(volume.activateRegion(region, selected), "activate 2x3x2 cells");
    require(selected == 12 && volume.count(0,0) == 12, "region reports selected cells");
    for (uint32_t index = 0; index < 12; ++index) {
        const g::Int3 position{int32_t(index % 2 * 2), int32_t(index / 2 % 3 * 2), int32_t(index / 6 * 2)};
        require(volume.findLeaf(position) == id(0,index), "x-fast region allocation order");
    }
    check(volume.finishTopology(), "finish full region");
    check(volume.activateRegion(region, selected), "repeat full region");
    require(selected == 12 && volume.count(0,0) == 12 && !volume.dirty(), "existing cells count without reallocating or dirtying");
    const auto voxels = extent(volume, 0, {0,0,0,0}, {2,2,2,0});
    check(volume.activateRegionFromMasks(voxels, nullptr, 0, selected), "level-zero region no-op");
    require(selected == 0, "level-zero no-op ignores missing selection data");

    g::HostVolume values;
    check(values.configure(shape({1,1,1,1})), "configure threshold fixture");
    float input[12]{};
    input[0] = .5f; input[1] = std::nextafter(.5f, std::numeric_limits<float>::infinity());
    input[2] = std::numeric_limits<float>::quiet_NaN(); input[3] = -std::numeric_limits<float>::infinity();
    input[4] = std::numeric_limits<float>::infinity(); input[9] = .75f;
    check(values.activateRegionFromValues(region, input, 12, .5f, selected), "strict threshold region");
    require(selected == 3 && values.findLeaf({2,0,0}) == id(0,0) && values.findLeaf({0,4,0}) == id(0,1) &&
            values.findLeaf({2,2,2}) == id(0,2), "threshold indices 1,4,9 map independently to x-fast cells");
    require(!values.isAllocated({0,0,0}) && !values.isAllocated({0,2,0}) && !values.isAllocated({2,2,0}),
            "equal, NaN and negative infinity samples do not pass finite threshold");
    const auto before = records(values);
    check(values.activateRegionFromValues(region, input, 12, .5f, selected), "repeat threshold region");
    require(selected == 3 && records(values) == before, "repeated selected cells preserve IDs and records");
    check(values.activateRegionFromValues(region, input, 12, std::numeric_limits<float>::quiet_NaN(), selected), "NaN threshold");
    require(selected == 0 && records(values) == before, "NaN threshold matches no values");
    require(NVRHI_FAILED(values.activateRegionFromValues(region, input, 11, .5f, selected)) && selected == 0 && records(values) == before,
            "short threshold source rejects before mutation");
    uint8_t masks[12]{}; masks[0] = 255; masks[11] = 2;
    check(values.activateRegionFromMasks(region, masks, 12, selected), "nonboolean mask selection");
    require(selected == 2 && values.findLeaf({0,0,0}) == id(0,3) && values.findLeaf({2,4,2}) == id(0,4),
            "any nonzero mask byte selects a cell");
    check(values.finishTopology(), "finish mixed selection fixture");

    g::HostVolume parents;
    check(parents.configure(shape({1,1,1})), "configure parent-only region");
    const auto parentRegion = extent(parents, 2, {0,0,0,0}, {8,4,4,0});
    check(parents.activateRegion(parentRegion, selected), "activate parent-level region");
    require(selected == 2 && parents.count(0,1) == 2 && parents.count(0,0) == 0,
            "region level selects its immediate children, not implicit leaves");
    check(parents.finishTopology(), "finish parent-only region");
    check(parents.activateHalo(parentRegion, selected), "parent-only halo has no occupied leaves");
    require(selected == 0, "legacy halo queries allocated leaf at cell origin");
    activate(parents, 0,0,0);
    check(parents.activateHalo(parentRegion, selected), "higher-level halo beside leaf");
    require(selected == 1 && parents.count(0,0) == 1 && parents.count(0,1) == 2,
            "higher-level halo selects existing parent without inventing a leaf");
    check(parents.activateHalo(parentRegion, selected), "repeat higher-level halo");
    require(selected == 1, "legacy higher-level selection count remains independent of allocated parent count");
}
void selectionMayBorrowGrowingPool() {
    g::HostVolume volume;
    check(volume.configure(shape({1,1,1,1,1,1,1})), "configure borrowed mask selection");
    const auto leaf = activate(volume, 0,0,0);
    const auto* masks = reinterpret_cast<const uint8_t*>(volume.node(leaf));
    std::array<uint8_t, sizeof(g::Node)> expected;
    std::memcpy(expected.data(), masks, expected.size());
    const auto region = extent(volume, 1, {0,0,0,0}, {128,2,2,0});
    uint64_t count = 0;
    check(volume.activateRegionFromMasks(region, masks, expected.size(), count), "selection source borrowed from growing node pool");
    uint64_t selected = 0;
    for (uint32_t i = 0; i < expected.size(); ++i) {
        selected += expected[i] != 0;
        require(volume.isAllocated({int32_t(i*2),0,0}) == (i == 0 || expected[i] != 0),
                "selection bytes were resolved before any pool pointer became stale");
    }
    require(count == selected, "borrowed selection count matches original bytes");
    check(volume.finishTopology(), "finish borrowed selection topology");
}
void simultaneousHalo() {
    g::HostVolume volume;
    check(volume.configure(shape({2,1,1,1,1})), "configure six-face halo");
    const auto center = activate(volume, 8,8,8);
    check(volume.setActive(center, false), "disable center atlas flag");
    const auto region = extent(volume, 1, {0,0,0,0}, {20,20,20,0});
    uint64_t selected = 0;
    check(volume.activateHalo(region, selected), "six-face simultaneous halo");
    require(selected == 6 && volume.count(0,0) == 7 && !volume.isActive({8,8,8}),
            "halo uses allocated inactive center and preserves its flag");
    const g::Int3 faces[] = {{4,8,8},{12,8,8},{8,4,8},{8,12,8},{8,8,4},{8,8,12}};
    for (g::Int3 face : faces) require(volume.isAllocated(face), "all six faces selected");
    require(!volume.isAllocated({4,4,8}) && !volume.isAllocated({4,4,4}) && !volume.isAllocated({0,8,8}),
            "one halo call excludes diagonals, corners and propagation through newly tagged cells");
    check(volume.activateHalo(region, selected), "second explicit halo layer");
    require(selected == 18 && volume.count(0,0) == 25, "second Manhattan layer has independent 18-cell count");
    check(volume.finishTopology(), "finish halo graph");
    require(volume.activeLeafCount() == 24, "inactive center remains excluded from atlas count");

    g::HostVolume shared;
    check(shared.configure(shape({2,1,1,1})), "configure shared-gap halo");
    activate(shared, 0,0,0); activate(shared, 8,0,0);
    const auto line = extent(shared, 1, {0,0,0,0}, {20,4,4,0});
    check(shared.activateHalo(line, selected), "shared-neighbor line halo");
    require(selected == 2 && shared.isAllocated({4,0,0}) && shared.isAllocated({12,0,0}) &&
            !shared.isAllocated({16,0,0}), "shared gap tagged once and right neighbor does not propagate");

    g::HostVolume signedVolume;
    check(signedVolume.configure(shape({2,1,1,1})), "configure signed halo");
    activate(signedVolume, -4,0,0);
    const auto signedLine = extent(signedVolume, 1, {-8,0,0,0}, {4,4,4,0});
    check(signedVolume.activateHalo(signedLine, selected), "signed cross-zero halo");
    require(selected == 2 && signedVolume.isAllocated({-8,0,0}) && signedVolume.isAllocated({0,0,0}),
            "signed neighbors use exact integer cell width");
    g::HostVolume external;
    check(external.configure(shape({2,1,1,1})), "configure outside-region neighbor");
    activate(external, 8,0,0);
    const auto oneCell = extent(external, 1, {4,0,0,0}, {8,4,4,0});
    check(external.activateHalo(oneCell, selected), "halo checks neighbors outside requested extent");
    require(selected == 1 && external.isAllocated({4,0,0}), "external occupied face influences selected cell");
}
void batchRollbackAndExtentValidation() {
    g::HostVolume volume;
    check(volume.configure(shape({1,1})), "configure batch extent failure");
    const auto original = activate(volume, 0,0,0);
    check(volume.finishTopology(), "finish batch baseline");
    const auto before = records(volume); const auto revision = volume.revision().topology;
    const auto region = extent(volume, 1, {0,0,0,0}, {6,2,2,0});
    uint64_t selected = 99;
    require(volume.activateRegion(region, selected) == nvrhi::FE_UNSUPPORTED && selected == 0,
            "last region point exceeds maximum after earlier cells succeeded");
    require(volume.root() == original && records(volume) == before && !volume.dirty() && volume.revision().topology == revision,
            "batch rolls back earlier successful activation and root growth");
    const auto halo = extent(volume, 1, {-2,0,0,0}, {4,2,2,0});
    require(volume.activateHalo(halo, selected) == nvrhi::FE_UNSUPPORTED && selected == 0 &&
            volume.root() == original && records(volume) == before && !volume.dirty(),
            "halo rolls back first signed neighbor when opposite neighbor cannot fit configured root");
    auto bad = region; bad.count = 2;
    require(NVRHI_FAILED(volume.activateRegion(bad, selected)) && selected == 0 && records(volume) == before,
            "malformed count rejects before iteration");
    bad = region; bad.cellWidth = 1;
    require(NVRHI_FAILED(volume.activateHalo(bad, selected)), "mismatched cell width rejected");
    bad = {1,2,{INT32_MAX,0,0},{INT32_MAX,0,0},{1,1,1},1};
    require(volume.activateRegion(bad, selected) == nvrhi::FE_UNSUPPORTED, "cell-to-position overflow rejected before conversion");
    const g::CellExtents empty{1,2,{0,0,0},{-1,0,0},{0,1,1},0};
    check(volume.activateRegionFromMasks(empty, nullptr, 0, selected), "empty selection accepts absent data");
    require(selected == 0 && records(volume) == before, "empty batch leaves topology intact");

    g::HostVolume budget({200 * sizeof(g::Node)});
    check(budget.configure(shape({1,1,1,1,1,1})), "configure large rollback journal");
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        g::NodeId parent;
        check(budget.activateAtLevel(1, {x*4,y*4,0}, parent), "preallocate independent parent cells");
    }
    check(budget.finishTopology(), "finish 64-parent baseline");
    const auto saved = records(budget); const auto savedRoot = budget.root();
    const auto many = extent(budget, 1, {0,0,0,0}, {32,32,2,0}); // 256 leaves; budget allows 200.
    require(budget.activateRegion(many, selected) == nvrhi::FE_OUT_OF_MEMORY && selected == 0,
            "large batch fails after touching more than fixed-journal parent count");
    require(records(budget) == saved && budget.root() == savedRoot && budget.count(0,0) == 0 &&
            budget.count(1,1) == 0 && !budget.dirty(), "unbounded batch journal restores every earlier parent and allocation");
    check(budget.validate(), "validate graph after large batch rollback");
}
void independentPointSet() {
    g::HostVolume volume;
    check(volume.configure(shape({2, 1, 1, 1, 1, 1, 1, 1})), "configure random signed point set");
    std::map<std::tuple<int, int, int>, g::NodeId> expected;
    uint32_t random = 0x573ae92u;
    for (unsigned iteration = 0; iteration < 800; ++iteration) {
        double p[3]; int origin[3];
        for (unsigned axis = 0; axis < 3; ++axis) {
            random = random * 1664525u + 1013904223u;
            p[axis] = int((random >> 16) % 61) - 30 + .375;
            origin[axis] = int(std::floor(p[axis] / 4)) * 4;
        }
        const auto result = activate(volume, p[0], p[1], p[2]);
        require(same(volume.node(result)->position, {origin[0], origin[1], origin[2]}), "independent floor-grid quantization");
        const auto key = std::make_tuple(origin[0], origin[1], origin[2]);
        auto [position, inserted] = expected.emplace(key, result);
        require(inserted || position->second == result, "repeated point keeps prior NodeId");
        if (iteration % 47 == 0) check(volume.finishTopology(), "periodic graph validation");
    }
    check(volume.finishTopology(), "finish independent point set");
    require(volume.activeLeafCount() == expected.size(), "active count matches independent unique voxel-brick set");
    for (const auto& entry : expected) {
        const auto [x, y, z] = entry.first;
        require(volume.findLeaf({x, y, z}) == entry.second && volume.isActive({x + 3, y + 3, z + 3}),
                "all earlier bricks survive growth and pool reallocations");
    }
    g::PackedHostTopology snapshot;
    check(volume.packTopology(snapshot), "pack independent point set");
    const auto saved = snapshot.nodes;
    check(volume.clear(), "clear after export");
    require(snapshot.nodes == saved && snapshot.activeLeafCount == expected.size(), "CPU export owns its bytes after source clear");
}
}
int main() {
    try {
        legacyPositive(); legacyNegative(); signedGrowthAndPromotion(); batchRootEnclosure(); allocationRollbackAndClear();
        manualValidationAndFlags(); mismatchedChildListValidation(); extentAndInputValidation();
        regionOrderingAndThresholds(); selectionMayBorrowGrowingPool(); simultaneousHalo(); batchRollbackAndExtentValidation(); independentPointSet();
        std::puts("GVDB HostVolume CPU semantics PASS: legacy ordering, signed growth, promotion, rollback, pools, empty export, validation, regions, halo");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "GVDB HostVolume CPU test FAIL: %s\n", e.what());
        return 1;
    }
}
