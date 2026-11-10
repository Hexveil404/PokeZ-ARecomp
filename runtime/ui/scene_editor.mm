// ZA SDK scene editor (the start screen's Editor page): Pokémon Z-A's world seen from above - every object of its
// scenes and every instance of its instanced models (most buildings) - picked, moved, turned, scaled, copied and
// deleted with the mouse, then saved as a mod (mods/<name>/edits.tsv -> mods/<name>/romfs, layered by
// runtime/services/fs.c like any mod).
// The world comes from the player's own files: zaprompts --scene-index (tools/zasdk/zasdk.py index) writes
// cache/sdk/world.tsv once (~1 min); a save runs zaprompts --scene-apply on the mod's edits.tsv. Nothing of the
// game is shipped: the SDK is the format knowledge (tools/zasdk/scene.py, scene_writer.py, trins.py).
// Keys: wheel zoom, right / middle drag pan, left click select (shift: add), left drag move, Del delete, Ctrl+D copy,
// Ctrl+Z undo, Q / E turn 15 degrees.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "../platform.h"
#include "../settings.h"

extern "C" bool launcher_zaprompts(char *out, size_t n);  // runtime/ui/launcher.mm: the helper's path
extern "C" const char *launcher_game_data(void);          // romfs.bin (or romfs/) of the set-up game, "" if none
extern "C" void launcher_mods_changed(void);              // the Mods page lists again

namespace {

const float kPi = 3.14159265358979f;

struct Obj {
    std::string scene, key, name, model;
    float tr[10];  // px py pz qx qy qz qw sx sy sz
    float orig[10];
    bool deleted = false, added = false;
    int cat = 0;  // 0 other, 1 building (_ar / _b), 2 prop (_pl), 3 item, 4 ground (_g)
};

struct State {
    std::vector<Obj> objs;
    bool loaded = false;
    std::string status;
    PlatProc proc = 0;
    int proc_kind = 0;  // 1 index, 2 apply
    float cx = -350, cz = -250, zoom = 1.5f;  // view centre (world x, z) and pixels per metre
    std::vector<int> sel;
    std::vector<std::vector<Obj>> undo;
    char mod_name[64] = "MyScene";
    char filter[64] = "";
    bool show[5] = {true, true, true, true, true};
    bool dragging = false;
    float drag_x = 0, drag_z = 0;
    int copies = 0;
} S;

const ImU32 kCat[5] = {IM_COL32(150, 160, 170, 170), IM_COL32(242, 181, 68, 210), IM_COL32(86, 182, 230, 190),
                       IM_COL32(110, 210, 140, 190), IM_COL32(120, 110, 200, 160)};
const char *kCatName[5] = {"Other", "Buildings", "Props", "Items", "Ground"};
const char *kCatNamePt[5] = {"Outros", "Construções", "Objetos", "Itens", "Chão"};

int category(const std::string &m) {
    if (m.find("field_item") != std::string::npos) return 3;
    auto has = [&](const char *s) { return m.find(s) != std::string::npos; };
    if (has("_ar/") || has("_b/") || has("_ar0") || has("_b_")) return 1;
    if (has("_pl/") || has("_pl0")) return 2;
    if (has("_g/") || has("_g0")) return 4;
    return 0;
}

bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

void load_index(const char *path) {
    S.objs.clear();
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *col[5] = {line};
        int n = 1;
        for (char *p = line; *p && n < 5; p++)
            if (*p == '\t') *p = 0, col[n++] = p + 1;
        if (n < 5) continue;
        Obj o;
        o.scene = col[0], o.key = col[1], o.name = col[2], o.model = col[3];
        if (sscanf(col[4], "%f %f %f %f %f %f %f %f %f %f", &o.tr[0], &o.tr[1], &o.tr[2], &o.tr[3], &o.tr[4], &o.tr[5],
                   &o.tr[6], &o.tr[7], &o.tr[8], &o.tr[9]) != 10) continue;
        memcpy(o.orig, o.tr, sizeof o.tr);
        o.cat = category(o.model);
        S.objs.push_back(std::move(o));
    }
    fclose(f);
    S.loaded = !S.objs.empty();
}

// a mod's earlier edits.tsv laid over the index (the editor picks up where it was saved)
void load_edits(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    std::unordered_map<std::string, int> at;
    for (int i = 0; i < (int)S.objs.size(); i++) at[S.objs[i].scene + "\t" + S.objs[i].key] = i;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *col[5] = {line};
        int n = 1;
        for (char *p = line; *p && n < 5; p++)
            if (*p == '\t') *p = 0, col[n++] = p + 1;
        if (n < 3) continue;
        auto it = at.find(std::string(col[1]) + "\t" + col[2]);
        if (it == at.end()) continue;
        Obj &o = S.objs[it->second];
        float t[10];
        bool has_t = n >= 4 && sscanf(col[3], "%f %f %f %f %f %f %f %f %f %f", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5],
                                      &t[6], &t[7], &t[8], &t[9]) == 10;
        if (!strcmp(col[0], "delete")) o.deleted = true;
        else if (!strcmp(col[0], "move") && has_t) memcpy(o.tr, t, sizeof t);
        else if (!strcmp(col[0], "copy") && has_t) {
            Obj c = o;
            c.added = true, c.deleted = false;
            memcpy(c.tr, t, sizeof t);
            S.objs.push_back(c);
        }
    }
    fclose(f);
}

bool save_edits(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "# ZA SDK edits (tools/zasdk/zasdk.py apply): op, scene, key, transform\n");
    auto tr = [&](const float *t) {
        static char b[256];
        snprintf(b, sizeof b, "%g %g %g %g %g %g %g %g %g %g", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9]);
        return b;
    };
    int n = 0;
    for (const Obj &o : S.objs) {
        if (o.added) {
            if (!o.deleted) fprintf(f, "copy\t%s\t%s\t%s\n", o.scene.c_str(), o.key.c_str(), tr(o.tr)), n++;
        } else if (o.deleted) fprintf(f, "delete\t%s\t%s\n", o.scene.c_str(), o.key.c_str()), n++;
        else if (memcmp(o.tr, o.orig, sizeof o.tr)) fprintf(f, "move\t%s\t%s\t%s\n", o.scene.c_str(), o.key.c_str(), tr(o.tr)), n++;
    }
    fclose(f);
    return n > 0;
}

void spawn(int kind, std::vector<std::string> args, const char *log) {
    char tool[1100];
    if (!launcher_zaprompts(tool, sizeof tool)) { S.status = "zaprompts not found"; return; }
    std::vector<const char *> av = {tool};
    for (auto &a : args) av.push_back(a.c_str());
    av.push_back(NULL);
    S.proc = plat_spawn(av.data(), log, false);
    S.proc_kind = S.proc ? kind : 0;
}

void push_undo() {
    S.undo.push_back(S.objs);
    if (S.undo.size() > 30) S.undo.erase(S.undo.begin());
}

float yaw_of(const float *t) {  // the turn around the vertical axis (degrees) of a quaternion
    return atan2f(2 * (t[6] * t[4] + t[3] * t[5]), 1 - 2 * (t[4] * t[4] + t[5] * t[5])) * 180 / kPi;
}
void set_yaw(float *t, float deg) {  // (only the vertical turn kept: the world's objects stand upright)
    float h = deg * kPi / 360;
    t[3] = 0, t[4] = sinf(h), t[5] = 0, t[6] = cosf(h);
}

}  // namespace

extern "C" void scene_editor_draw(ImVec2 a, ImVec2 b, float s, bool pt) {
    auto T = [&](const char *en, const char *ptx) { return pt ? ptx : en; };
    // ---- the helper's run (index or apply)
    if (S.proc) {
        int r = plat_proc_wait(S.proc, false);
        if (r >= 0) {
            int kind = S.proc_kind;
            S.proc = 0, S.proc_kind = 0;
            if (kind == 1) {
                load_index("cache/sdk/world.tsv");
                S.status = S.loaded ? T("World loaded.", "Mundo carregado.") : T("Reading the world failed (cache/sdk/index.log).", "Falhou ao ler o mundo (cache/sdk/index.log).");
            } else {
                S.status = r == 0 ? std::string(T("Mod saved: ", "Mod salvo: ")) + S.mod_name +
                                        T(" - turn it on in Mods, then Play.", " - ligue em Mods e depois Jogar.")
                                  : T("Saving the mod failed (see apply.log in the mod folder).", "Falhou ao salvar o mod (veja apply.log na pasta do mod).");
                launcher_mods_changed();
            }
        }
    }
    if (!S.loaded && !S.proc) {
        if (file_exists("cache/sdk/world.tsv")) load_index("cache/sdk/world.tsv");
        if (!S.loaded) {
            ImGui::SetCursorScreenPos(ImVec2(a.x + 20 * s, a.y + 20 * s));
            ImGui::BeginGroup();
            ImGui::TextWrapped("%s", T("Scene editor (SDK, experimental): move, turn, copy and delete the world's buildings, "
                                       "props and ground pieces, and save it as a mod. It first reads the world from your own "
                                       "game files (about a minute, once).",
                                       "Editor de cenário (SDK, experimental): mova, gire, copie e apague construções, objetos "
                                       "e pedaços de chão do mundo, e salve como mod. Primeiro ele lê o mundo dos seus próprios "
                                       "arquivos do jogo (cerca de um minuto, uma vez)."));
            const char *data = launcher_game_data();
            ImGui::BeginDisabled(!data[0]);
            if (ImGui::Button(T("Read the world", "Ler o mundo"))) {
                SDL_CreateDirectory("cache/sdk");
                spawn(1, {"--romfs", data, "--scene-index", "cache/sdk/world.tsv"}, "cache/sdk/index.log");
                S.status = T("Reading the world...", "Lendo o mundo...");
            }
            ImGui::EndDisabled();
            if (!data[0]) ImGui::TextDisabled("%s", T("Set the game up first (Play page).", "Prepare o jogo primeiro (página Jogar)."));
            ImGui::EndGroup();
        }
    }
    if (!S.status.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + 20 * s, b.y - 26 * s));
        ImGui::TextColored(ImVec4(0.95f, 0.71f, 0.27f, 1), "%s", S.status.c_str());
    }
    if (!S.loaded) return;

    // ---- layout: the map on the left, the panel on the right
    float pw = 330 * s;
    ImVec2 ma = ImVec2(a.x, a.y), mb = ImVec2(b.x - pw - 10 * s, b.y - 34 * s);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ma, mb, IM_COL32(6, 14, 24, 230), 8 * s);
    dl->PushClipRect(ma, mb, true);
    ImVec2 mc = ImVec2((ma.x + mb.x) / 2, (ma.y + mb.y) / 2);
    auto to_screen = [&](float x, float z) { return ImVec2(mc.x + (x - S.cx) * S.zoom * s, mc.y + (z - S.cz) * S.zoom * s); };
    auto to_world = [&](ImVec2 p, float *x, float *z) { *x = S.cx + (p.x - mc.x) / (S.zoom * s); *z = S.cz + (p.y - mc.y) / (S.zoom * s); };
    // grid every 50 m
    for (int k = -40; k <= 40; k++) {
        float g = k * 50.0f;
        ImVec2 p0 = to_screen(g, S.cz - 2000), p1 = to_screen(g, S.cz + 2000), q0 = to_screen(S.cx - 2000, g), q1 = to_screen(S.cx + 2000, g);
        dl->AddLine(p0, p1, IM_COL32(255, 255, 255, 10)), dl->AddLine(q0, q1, IM_COL32(255, 255, 255, 10));
    }
    std::string flt = S.filter;
    std::vector<char> is_sel(S.objs.size(), 0);
    for (int i : S.sel) if (i < (int)is_sel.size()) is_sel[i] = 1;
    for (size_t i = 0; i < S.objs.size(); i++) {
        const Obj &o = S.objs[i];
        if (o.deleted || !S.show[o.cat]) continue;
        if (!flt.empty() && o.model.find(flt) == std::string::npos && o.name.find(flt) == std::string::npos) continue;
        ImVec2 p = to_screen(o.tr[0], o.tr[2]);
        if (p.x < ma.x - 20 || p.x > mb.x + 20 || p.y < ma.y - 20 || p.y > mb.y + 20) continue;
        float r = (o.cat == 1 ? 3.0f : 1.4f) * fmaxf(0.4f, fminf(S.zoom, 3.0f)) * s;
        float yaw = yaw_of(o.tr) * kPi / 180, c = cosf(yaw), sn = sinf(yaw);
        ImVec2 q[4];
        float hx[4] = {-r, r, r, -r}, hz[4] = {-r * 0.6f, -r * 0.6f, r * 0.6f, r * 0.6f};
        for (int k = 0; k < 4; k++) q[k] = ImVec2(p.x + hx[k] * c - hz[k] * sn, p.y + hx[k] * sn + hz[k] * c);
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], is_sel[i] ? IM_COL32(255, 80, 80, 255) : o.added ? IM_COL32(255, 255, 255, 230) : kCat[o.cat]);
    }
    dl->PopClipRect();

    // ---- mouse on the map
    ImGui::SetCursorScreenPos(ma);
    ImGui::InvisibleButton("##map", ImVec2(mb.x - ma.x, mb.y - ma.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGuiIO &io = ImGui::GetIO();
    bool hover = ImGui::IsItemHovered();
    if (hover && io.MouseWheel != 0) {  // zoom on the cursor
        float wx, wz;
        to_world(io.MousePos, &wx, &wz);
        S.zoom = fminf(40, fmaxf(0.05f, S.zoom * powf(1.2f, io.MouseWheel)));
        float nx, nz;
        to_world(io.MousePos, &nx, &nz);
        S.cx += wx - nx, S.cz += wz - nz;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
        S.cx -= io.MouseDelta.x / (S.zoom * s), S.cz -= io.MouseDelta.y / (S.zoom * s);
    if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {  // pick the nearest within 10 px
        float best = 10 * s, bx = 0;
        int pick = -1;
        for (size_t i = 0; i < S.objs.size(); i++) {
            const Obj &o = S.objs[i];
            if (o.deleted || !S.show[o.cat]) continue;
            if (!flt.empty() && o.model.find(flt) == std::string::npos && o.name.find(flt) == std::string::npos) continue;
            ImVec2 p = to_screen(o.tr[0], o.tr[2]);
            float d = hypotf(p.x - io.MousePos.x, p.y - io.MousePos.y);
            if (d < best) best = d, pick = (int)i;
        }
        (void)bx;
        if (!io.KeyShift) S.sel.clear();
        if (pick >= 0) {
            bool already = false;
            for (int i : S.sel) already |= i == pick;
            if (!already) S.sel.push_back(pick);
            S.dragging = true;
            to_world(io.MousePos, &S.drag_x, &S.drag_z);
            push_undo();
        }
    }
    if (S.dragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float wx, wz;
            to_world(io.MousePos, &wx, &wz);
            for (int i : S.sel) S.objs[i].tr[0] += wx - S.drag_x, S.objs[i].tr[2] += wz - S.drag_z;
            S.drag_x = wx, S.drag_z = wz;
        } else S.dragging = false;
    }
    bool typing = io.WantTextInput;
    if (!typing && !S.sel.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            push_undo();
            for (int i : S.sel) S.objs[i].deleted = true;
            S.sel.clear();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
            push_undo();
            std::vector<int> ns;
            for (int i : S.sel) {
                Obj c = S.objs[i];
                c.added = true, c.deleted = false;
                c.tr[0] += 8;  // (next to it: dragged into place after)
                S.objs.push_back(c);
                ns.push_back((int)S.objs.size() - 1);
            }
            S.sel = ns;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Q) || ImGui::IsKeyPressed(ImGuiKey_E)) {
            push_undo();
            for (int i : S.sel) set_yaw(S.objs[i].tr, yaw_of(S.objs[i].tr) + (ImGui::IsKeyPressed(ImGuiKey_Q) ? -15.0f : 15.0f));
        }
    }
    if (!typing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !S.undo.empty()) {
        S.objs = S.undo.back();
        S.undo.pop_back();
        S.sel.clear();
    }

    // ---- the panel
    ImGui::SetCursorScreenPos(ImVec2(b.x - pw, a.y));
    ImGui::BeginChild("##sdkpanel", ImVec2(pw, b.y - a.y - 34 * s), ImGuiChildFlags_None);
    ImGui::TextUnformatted(T("Show", "Mostrar"));
    for (int k = 0; k < 5; k++) {
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(kCat[k]));
        ImGui::Checkbox(pt ? kCatNamePt[k] : kCatName[k], &S.show[k]);
        ImGui::PopStyleColor();
        if (k != 2 && k != 4) ImGui::SameLine();
    }
    ImGui::InputTextWithHint("##flt", T("Filter by model / name", "Filtrar por modelo / nome"), S.filter, sizeof S.filter);
    ImGui::Separator();
    int nd = 0, na = 0, nm = 0;
    for (const Obj &o : S.objs) nd += !o.added && o.deleted, na += o.added && !o.deleted, nm += !o.added && !o.deleted && memcmp(o.tr, o.orig, sizeof o.tr);
    ImGui::TextWrapped(T("%d objects - moved %d, copies %d, deleted %d", "%d objetos - movidos %d, cópias %d, apagados %d"),
                       (int)S.objs.size(), nm, na, nd);
    ImGui::Separator();
    if (S.sel.size() == 1) {
        Obj &o = S.objs[S.sel[0]];
        ImGui::TextWrapped("%s", o.name.c_str());
        ImGui::TextDisabled("%s", o.model.c_str());
        float pos[3] = {o.tr[0], o.tr[1], o.tr[2]}, yaw = yaw_of(o.tr), sc = o.tr[7];
        bool ch = false;
        ch |= ImGui::DragFloat3(T("Position", "Posição"), pos, 0.1f);
        ch |= ImGui::DragFloat(T("Turn", "Giro"), &yaw, 1, -180, 180, "%.0f deg");
        ch |= ImGui::DragFloat(T("Scale", "Escala"), &sc, 0.01f, 0.05f, 20);
        if (ImGui::IsItemActivated()) push_undo();
        if (ch) {
            o.tr[0] = pos[0], o.tr[1] = pos[1], o.tr[2] = pos[2];
            if (fabsf(yaw - yaw_of(o.tr)) > 0.01f) set_yaw(o.tr, yaw);
            if (sc > 0 && fabsf(sc - o.tr[7]) > 1e-4f) { float k = sc / o.tr[7]; o.tr[7] *= k, o.tr[8] *= k, o.tr[9] *= k; }
        }
    } else if (S.sel.size() > 1) ImGui::Text(T("%d selected", "%d selecionados"), (int)S.sel.size());
    else ImGui::TextDisabled("%s", T("Click an object on the map.", "Clique num objeto no mapa."));
    if (!S.sel.empty()) {
        if (ImGui::Button(T("Copy (Ctrl+D)", "Copiar (Ctrl+D)"))) {
            push_undo();
            std::vector<int> ns;
            for (int i : S.sel) { Obj c = S.objs[i]; c.added = true; c.tr[0] += 8; S.objs.push_back(c); ns.push_back((int)S.objs.size() - 1); }
            S.sel = ns;
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Delete (Del)", "Apagar (Del)"))) {
            push_undo();
            for (int i : S.sel) S.objs[i].deleted = true;
            S.sel.clear();
        }
    }
    ImGui::BeginDisabled(S.undo.empty());
    if (ImGui::Button(T("Undo (Ctrl+Z)", "Desfazer (Ctrl+Z)"))) { S.objs = S.undo.back(); S.undo.pop_back(); S.sel.clear(); }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", T("Wheel: zoom - right drag: pan - drag: move - Q / E: turn - Shift+click: add to selection",
                                "Roda: zoom - arrastar com o direito: mover o mapa - arrastar: mover - Q / E: girar - Shift+clique: somar à seleção"));
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::InputText(T("Mod name", "Nome do mod"), S.mod_name, sizeof S.mod_name);
    std::string dir = std::string("mods/") + S.mod_name;
    if (ImGui::Button(T("Open this mod's edits", "Abrir as edições deste mod"))) {
        load_index("cache/sdk/world.tsv");
        load_edits((dir + "/edits.tsv").c_str());
        S.undo.clear(), S.sel.clear();
        S.status = T("Edits loaded.", "Edições carregadas.");
    }
    ImGui::BeginDisabled(S.proc != 0 || !S.mod_name[0]);
    if (ImGui::Button(T("Save as mod", "Salvar como mod"), ImVec2(-1, 0))) {
        SDL_CreateDirectory("mods");
        SDL_CreateDirectory(dir.c_str());
        if (!save_edits((dir + "/edits.tsv").c_str())) S.status = T("Nothing changed yet.", "Nada mudou ainda.");
        else {
            spawn(2, {"--romfs", launcher_game_data(), "--scene-apply", dir + "/edits.tsv", "--out", dir}, (dir + "/apply.log").c_str());
            S.status = T("Saving the mod...", "Salvando o mod...");
        }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}
