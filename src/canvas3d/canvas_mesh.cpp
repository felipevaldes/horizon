#include "canvas_mesh.hpp"
#include "board/board.hpp"
#include "board/board_layers.hpp"
#include "poly2tri/poly2tri.h"
#include "logger/logger.hpp"
#include "util/geom_util.hpp"
#include "util/min_max_accumulator.hpp"
#include <future>
#include <thread>
#include <atomic>

namespace horizon {
CanvasMesh::CanvasMesh() : ca(CanvasPatch::SimplifyOnUpdate::NO)
{
}

void CanvasMesh::update(const Board &b)
{
    update_only(b);
    prepare_only();
}

void CanvasMesh::update_only(const Board &b)
{
    ca.update(b);
    prepare(b);
}

void CanvasMesh::prepare_only(std::function<void()> cb)
{
    cancel = false;
    ca.simplify();
    if (cb)
        cb();
    prepare_work(cb);
}

void CanvasMesh::Layer3D::move_from(Layer3D &&other)
{
    tris = std::move(other.tris);
    walls = std::move(other.walls);
}

void CanvasMesh::Layer3D::copy_sizes_from(const Layer3D &other)
{
    alpha = other.alpha;
    offset = other.offset;
    thickness = other.thickness;
    explode_mul = other.explode_mul;
}

void CanvasMesh::prepare_worker(std::atomic_size_t &layer_counter, std::function<void()> cb)
{
    size_t i;
    const auto n = layers_to_prepare.size();
    while ((i = layer_counter.fetch_add(1, std::memory_order_relaxed)) < n) {
        if (cancel)
            return;
        const auto layer = layers_to_prepare.at(i);
        if (layer == BoardLayers::TOP_MASK || layer == BoardLayers::BOTTOM_MASK) {
            prepare_soldermask(layer);
        }
        else if (layer == BoardLayers::TOP_SILKSCREEN) {
            prepare_silkscreen(layer, BoardLayers::TOP_MASK);
        }
        else if (layer == BoardLayers::BOTTOM_SILKSCREEN) {
            prepare_silkscreen(layer, BoardLayers::BOTTOM_MASK);
        }
        else if (layer == 20000) {
            for (const auto &it : ca.get_patches()) {
                if (it.first.layer == 10000 && it.first.type == PatchType::HOLE_PTH) {
                    ClipperLib::ClipperOffset ofs;
                    ofs.AddPaths(it.second, ClipperLib::jtRound, ClipperLib::etClosedPolygon);
                    ClipperLib::Paths res;
                    ofs.Execute(res, -.001_mm);
                    for (const auto &path : res) {
                        add_path(layer, path);
                    }
                }
            }
        }
        else if (layer >= 10000) {
            // nop will be cloned from outline layer
        }
        else {
            prepare_layer(layer);
            if (layer == BoardLayers::L_OUTLINE) {
                for (auto l : layers_to_prepare) {
                    if (l >= 10'000 && l < 11'000) {
                        layers.at(l).tris = layers.at(layer).tris;
                        layers.at(l).walls = layers.at(layer).walls;
                    }
                }
            }
        }
        layers.at(layer).done = true;
        if (cb)
            cb();
    }
}

void CanvasMesh::prepare(const Board &brd)
{
    layers.clear();

    float board_thickness = -((float)brd.stackup.at(0).thickness);
    int n_inner_layers = brd.get_n_inner_layers();
    for (const auto &it : brd.stackup) {
        board_thickness += it.second.thickness + it.second.substrate_thickness;
    }
    board_thickness /= 1e6;

    layers_to_prepare.clear();
    int layer = BoardLayers::TOP_COPPER;
    layers[layer].offset = 0;
    layers[layer].thickness = brd.stackup.at(0).thickness / 1e6;
    layers[layer].explode_mul = 1;
    layers_to_prepare.push_back(layer);

    layer = BoardLayers::BOTTOM_COPPER;
    layers[layer].offset = -board_thickness;
    layers[layer].thickness = +(brd.stackup.at(layer).thickness / 1e6);
    layers[layer].explode_mul = -2 * n_inner_layers - 1;
    layers_to_prepare.push_back(layer);

    {
        float offset = -(brd.stackup.at(0).substrate_thickness / 1e6);
        for (int i = 0; i < n_inner_layers; i++) {
            layer = -i - 1;
            layers[layer].offset = offset;
            layers[layer].thickness = -(brd.stackup.at(layer).thickness / 1e6);
            layers[layer].explode_mul = -1 - 2 * i;
            offset -= brd.stackup.at(layer).thickness / 1e6 + brd.stackup.at(layer).substrate_thickness / 1e6;
            layers_to_prepare.push_back(layer);
        }
    }

    layer = BoardLayers::L_OUTLINE;
    layers[layer].offset = 0;
    layers[layer].thickness = -(brd.stackup.at(0).substrate_thickness / 1e6);
    layers[layer].explode_mul = 0;
    layers_to_prepare.push_back(layer);

    float offset = -(brd.stackup.at(0).substrate_thickness / 1e6);
    for (int i = 0; i < n_inner_layers; i++) {
        int l = 10000 + i;
        offset -= brd.stackup.at(-i - 1).thickness / 1e6;
        layers[l].offset = offset;
        layers[l].thickness = -(brd.stackup.at(-i - 1).substrate_thickness / 1e6);
        layers[l].explode_mul = -2 - 2 * i;

        offset -= brd.stackup.at(-i - 1).substrate_thickness / 1e6;
        layers_to_prepare.push_back(l);
    }

    layer = BoardLayers::TOP_MASK;
    layers[layer].offset = brd.stackup.at(0).thickness / 1e6 + 1e-3;
    layers[layer].thickness = 0.01;
    layers[layer].alpha = .8;
    layers[layer].explode_mul = 3;
    layers_to_prepare.push_back(layer);

    layer = BoardLayers::BOTTOM_MASK;
    layers[layer].offset = -board_thickness - 1e-3;
    layers[layer].thickness = 0.035;
    layers[layer].alpha = .8;
    layers[layer].explode_mul = -2 * n_inner_layers - 3;
    layers_to_prepare.push_back(layer);

    layer = BoardLayers::TOP_SILKSCREEN;
    layers[layer].offset = brd.stackup.at(0).thickness / 1e6 + 1e-3;
    layers[layer].thickness = 0.035;
    layers[layer].explode_mul = 4;
    layers_to_prepare.push_back(layer);

    layer = BoardLayers::BOTTOM_SILKSCREEN;
    layers[layer].offset = -board_thickness - .1e-3;
    layers[layer].thickness = -0.035;
    layers[layer].explode_mul = -2 * n_inner_layers - 4;
    layers_to_prepare.push_back(layer);

    layer = BoardLayers::TOP_PASTE;
    layers[layer].offset = brd.stackup.at(0).thickness / 1e6 + 1e-3;
    layers[layer].thickness = 0.035;
    layers[layer].explode_mul = 2;
    layers_to_prepare.push_back(layer);

    layer = BoardLayers::BOTTOM_PASTE;
    layers[layer].offset = -board_thickness;
    layers[layer].thickness = -0.035;
    layers[layer].explode_mul = -2 * n_inner_layers - 2;
    layers_to_prepare.push_back(layer);


    layer = 20000; // pth holes
    layers[layer].offset = 0;
    layers[layer].thickness = -board_thickness;
    layers_to_prepare.push_back(layer);
}

void CanvasMesh::prepare_work(std::function<void()> cb)
{
    std::atomic_size_t layer_counter = 0;
    std::vector<std::future<void>> results;
    for (size_t i = 0; i < std::thread::hardware_concurrency(); i++) {
        results.push_back(
                std::async(std::launch::async, &CanvasMesh::prepare_worker, this, std::ref(layer_counter), cb));
    }
    for (auto &it : results) {
        it.wait();
    }
}


void CanvasMesh::prepare_soldermask(int layer)
{
    ClipperLib::Paths temp;
    {
        ClipperLib::Clipper cl;
        for (const auto &it : ca.get_patches()) {
            if (it.first.layer == BoardLayers::L_OUTLINE) { // add outline
                cl.AddPaths(it.second, ClipperLib::ptSubject, true);
            }
            else if (it.first.layer == layer) {
                cl.AddPaths(it.second, ClipperLib::ptClip, true);
            }
        }

        cl.Execute(ClipperLib::ctDifference, temp, ClipperLib::pftEvenOdd, ClipperLib::pftNonZero);
    }
    ClipperLib::PolyTree pt;
    ClipperLib::ClipperOffset cl;
    cl.AddPaths(temp, ClipperLib::jtSquare, ClipperLib::etClosedPolygon);
    cl.Execute(pt, -.001_mm);

    for (const auto node : pt.Childs) {
        polynode_to_tris(node, layer);
    }
}

void CanvasMesh::prepare_silkscreen(int layer, int soldermask_layer)
{
    ClipperLib::Paths result;
    {

        ClipperLib::Clipper cl;
        for (const auto &it : ca.get_patches()) {
            if (it.first.layer == layer) {
                cl.AddPaths(it.second, ClipperLib::ptSubject, true);
            }
        }
        cl.Execute(ClipperLib::ctUnion, result, ClipperLib::pftNonZero);
    }

    ClipperLib::Paths holes_without_soldermask;
    {
        ClipperLib::Clipper cl;
        for (const auto &it : ca.get_patches()) {
            if (it.first.layer == 10000
                && (it.first.type == PatchType::HOLE_NPTH || it.first.type == PatchType::HOLE_PTH)) {
                cl.AddPaths(it.second, ClipperLib::ptSubject, true);
            }
            else if (it.first.layer == soldermask_layer) {
                cl.AddPaths(it.second, ClipperLib::ptClip, true);
            }
        }
        cl.Execute(ClipperLib::ctIntersection, holes_without_soldermask, ClipperLib::pftNonZero,
                   ClipperLib::pftNonZero);
    }

    ClipperLib::Paths result_with_holes;
    {
        ClipperLib::Clipper cl;
        cl.AddPaths(result, ClipperLib::ptSubject, true);
        cl.AddPaths(holes_without_soldermask, ClipperLib::ptClip, true);
        cl.Execute(ClipperLib::ctDifference, result_with_holes, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    }

    ClipperLib::PolyTree pt;
    {
        ClipperLib::ClipperOffset cl;
        cl.AddPaths(result_with_holes, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
        cl.Execute(pt, -100); // .1um
    }

    for (const auto node : pt.Childs) {
        polynode_to_tris(node, layer);
    }
}

void CanvasMesh::prepare_layer(int layer)
{
    ClipperLib::Paths result;
    auto pft = ClipperLib::pftNonZero;
    {

        ClipperLib::Clipper cl;
        for (const auto &it : ca.get_patches()) {
            if (it.first.layer == layer) {
                cl.AddPaths(it.second, ClipperLib::ptSubject, true);
            }
        }

        if (layer == BoardLayers::L_OUTLINE) {
            pft = ClipperLib::pftEvenOdd;
        }
        cl.Execute(ClipperLib::ctUnion, result, pft);
    }

    ClipperLib::Paths result_with_holes;
    {
        ClipperLib::Clipper cl;
        cl.AddPaths(result, ClipperLib::ptSubject, true);
        for (const auto &it : ca.get_patches()) {
            if (it.first.layer == 10000
                && (it.first.type == PatchType::HOLE_NPTH || it.first.type == PatchType::HOLE_PTH)) {
                cl.AddPaths(it.second, ClipperLib::ptClip, true);
            }
        }
        cl.Execute(ClipperLib::ctDifference, result_with_holes, pft, ClipperLib::pftNonZero);
    }
    ClipperLib::PolyTree pt;
    {
        ClipperLib::ClipperOffset cl;
        cl.AddPaths(result_with_holes, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
        cl.Execute(pt, -100); // .1um
    }

    for (const auto node : pt.Childs) {
        polynode_to_tris(node, layer);
    }
}


void CanvasMesh::add_path(int layer, const ClipperLib::Path &path)
{
    if (path.size() >= 3) {
        layers.at(layer).walls.emplace_back(path.back().X, path.back().Y);
        for (size_t i = 0; i < path.size(); i++) {
            layers.at(layer).walls.emplace_back(path[i].X, path[i].Y);
        }
        layers.at(layer).walls.emplace_back(path[0].X, path[0].Y);
        layers.at(layer).walls.emplace_back(path[1].X, path[1].Y);
        layers.at(layer).walls.emplace_back(NAN, NAN);
    }
}

static void append_path(std::vector<p2t::Point> &store, std::vector<p2t::Point *> &out,
                        std::set<std::pair<ClipperLib::cInt, ClipperLib::cInt>> &point_set,
                        const ClipperLib::Path &path)
{
    for (const auto &it : path) {
        auto p = std::make_pair(it.X, it.Y);
        bool a = false;
        bool fixed = false;
        while (point_set.count(p)) {
            fixed = true;
            if (a)
                p.first++;
            else
                p.second++;
            a = !a;
        }
        if (fixed) {
            Logger::log_warning("fixed duplicate point", Logger::Domain::BOARD,
                                "at " + coord_to_string(Coordf(it.X, it.Y)));
        }
        point_set.insert(p);
        store.emplace_back(p.first, p.second);
        out.push_back(&store.back());
    }
}

void CanvasMesh::polynode_to_tris(const ClipperLib::PolyNode *node, int layer)
{
    assert(node->IsHole() == false);

    std::vector<p2t::Point> point_store;
    size_t pts_total = node->Contour.size();
    for (const auto child : node->Childs)
        pts_total += child->Contour.size();
    point_store.reserve(pts_total); // important so that iterators won't get invalidated
    std::set<std::pair<ClipperLib::cInt, ClipperLib::cInt>> point_set;

    try {
        std::vector<p2t::Point *> contour;
        contour.reserve(node->Contour.size());
        append_path(point_store, contour, point_set, node->Contour);
        p2t::CDT cdt(contour);
        for (const auto child : node->Childs) {
            std::vector<p2t::Point *> hole;
            hole.reserve(child->Contour.size());
            append_path(point_store, hole, point_set, child->Contour);
            cdt.AddHole(hole);
        }
        cdt.Triangulate();
        auto tris = cdt.GetTriangles();

        for (const auto &tri : tris) {
            for (int i = 0; i < 3; i++) {
                auto p = tri->GetPoint(i);
                layers.at(layer).tris.emplace_back(p->x, p->y);
            }
        }
    }
    catch (const std::runtime_error &e) {
        Logger::log_critical("error triangulating layer " + BoardLayers::get_layer_name(layer), Logger::Domain::BOARD,
                             e.what());
    }
    catch (...) {
        Logger::log_critical("error triangulating layer" + BoardLayers::get_layer_name(layer), Logger::Domain::BOARD,
                             "unspecified error");
    }

    layers.at(layer).walls.reserve(pts_total);
    add_path(layer, node->Contour);
    for (auto child : node->Childs) {
        add_path(layer, child->Contour);
    }

    for (auto child : node->Childs) {
        assert(child->IsHole() == true);
        for (auto child2 : child->Childs) { // add fragments in holes
            polynode_to_tris(child2, layer);
        }
    }
}

std::pair<Coordi, Coordi> CanvasMesh::get_bbox() const
{
    MinMaxAccumulator<int64_t> acc_x, acc_y;
    for (const auto &[key, paths] : ca.get_patches()) {
        for (const auto &path : paths) {
            for (const auto &p : path) {
                acc_x.accumulate(p.X);
                acc_y.accumulate(p.Y);
            }
        }
    }
    return {{acc_x.get_min(), acc_y.get_min()}, {acc_x.get_max(), acc_y.get_max()}};
}

} // namespace horizon
