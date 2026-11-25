// Native checks of the installer's portable half (tools/install_ui.h), and
// pictures of its screens.
//
//   c++ -std=c++20 -O2 -IUMVC3Hook/code tools/test_install.cpp
//       UMVC3Hook/code/space_bg.cpp -o build/test_install   (one command)
//   build/test_install [output-dir]
//
// With an output directory it writes each screen as a .ppm, drawn at the
// installer's default size with the real backdrop. The installer's text is
// drawn by Windows, so here every run of text is a box of about its size.

#include "install_ui.h"

#include <cstdio>
#include <cstdlib>

using namespace install;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (ok) return;
    ++g_failures;
    fprintf(stderr, "FAIL: %s\n", what);
}

void TestVdf()
{
    const std::string library =
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
        "\t\t\"label\"\t\t\"\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"357190\"\t\t\"123\"\n\t\t}\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"PATH\"\t\t\"/media/games/SteamLibrary\"\n\t}\n}\n";
    const std::vector<std::string> paths = VdfValues(library, "path");
    Check(paths.size() == 2, "two library paths");
    Check(paths.size() == 2 && paths[0] == "C:\\Program Files (x86)\\Steam", "escaped Windows path");
    Check(paths.size() == 2 && paths[1] == "/media/games/SteamLibrary", "key matched without case");
    // A key that opens a block has no value; a value that equals the key is not a key.
    Check(VdfValues(library, "apps").empty(), "block key has no value");
    Check(VdfValues("\"a\" \"path\" \"path\" \"x\"", "path").size() == 1, "value equal to the key");

    const std::string manifest =
        "\"AppState\"\n{\n\t\"appid\"\t\t\"357190\"\n\t\"name\"\t\t\"ULTIMATE MARVEL VS. CAPCOM 3\"\n"
        "\t\"StateFlags\"\t\t\"4\"\n\t\"installdir\"\t\t\"ULTIMATE MARVEL VS. CAPCOM 3\"\n}\n";
    const std::vector<std::string> dir = VdfValues(manifest, "installdir");
    Check(dir.size() == 1 && dir[0] == "ULTIMATE MARVEL VS. CAPCOM 3", "installdir");
}

void TestOverride()
{
    const std::string head = "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21\n\n"
                             "#arch=win64\n\n[Software\\\\Wine\\\\DllOverrides] 1700000000\n"
                             "#time=1da0000000\n\"d3d9\"=\"native\"\n";
    const std::string wanted =
        "\n[Software\\\\Wine\\\\AppDefaults\\\\umvc3.exe\\\\DllOverrides] 1800000000\n"
        "\"dinput8\"=\"native,builtin\"\n";

    std::string fresh = head;
    Check(AddWineDllOverride(fresh, "umvc3.exe", "dinput8", 1800000000) == OverrideResult::Added,
          "override added to a prefix without the key");
    Check(fresh == head + wanted, "new key appended whole");
    Check(AddWineDllOverride(fresh, "umvc3.exe", "dinput8", 1) == OverrideResult::Present,
          "second run changes nothing");
    Check(fresh == head + wanted, "second run left the file alone");

    // The key exists (in another case) with other values, and more keys follow.
    const std::string existing = head +
        "\n[software\\\\wine\\\\appdefaults\\\\UMVC3.exe\\\\dlloverrides] 1700000001\n"
        "#time=1da0000001\n\"xinput1_3\"=\"builtin\"\n"
        "\n[Software\\\\Zeta] 1700000002\n\"dinput8\"=\"elsewhere\"\n";
    std::string merged = existing;
    Check(AddWineDllOverride(merged, "umvc3.exe", "dinput8", 5) == OverrideResult::Added,
          "override added to an existing key");
    Check(merged.find("#time=1da0000001\n\"dinput8\"=\"native,builtin\"\n\"xinput1_3\"") !=
              std::string::npos,
          "value placed under the key's time line");
    Check(merged.size() == existing.size() + strlen("\"dinput8\"=\"native,builtin\"\n"),
          "nothing else changed");

    std::string chosen = head +
        "\n[Software\\\\Wine\\\\AppDefaults\\\\umvc3.exe\\\\DllOverrides] 1700000001\n"
        "\"DINPUT8\"=\"builtin\"\n";
    const std::string chosenBefore = chosen;
    Check(AddWineDllOverride(chosen, "umvc3.exe", "dinput8", 5) == OverrideResult::Present &&
              chosen == chosenBefore,
          "a value the user set is kept");

    std::string other = "REGEDIT4\n";
    Check(AddWineDllOverride(other, "umvc3.exe", "dinput8", 5) == OverrideResult::NotRegistry,
          "unknown file refused");
}

void Put16(std::vector<unsigned char>& out, unsigned v)
{
    out.push_back(v & 255);
    out.push_back((v >> 8) & 255);
}
void Put32(std::vector<unsigned char>& out, unsigned v)
{
    Put16(out, v & 0xffff);
    Put16(out, v >> 16);
}

// A stored archive of the given files, as `zip -0` writes one.
std::vector<unsigned char> MakeZip(const std::vector<std::pair<std::string, std::string>>& files,
                                   unsigned method = 0)
{
    std::vector<unsigned char> zip, central;
    for (const auto& [name, data] : files) {
        const unsigned local = static_cast<unsigned>(zip.size());
        Put32(zip, 0x04034b50);
        Put16(zip, 10); Put16(zip, 0); Put16(zip, method); Put32(zip, 0); Put32(zip, 0);
        Put32(zip, static_cast<unsigned>(data.size())); Put32(zip, static_cast<unsigned>(data.size()));
        Put16(zip, static_cast<unsigned>(name.size())); Put16(zip, 0);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), data.begin(), data.end());
        Put32(central, 0x02014b50);
        Put16(central, 30); Put16(central, 10); Put16(central, 0); Put16(central, method);
        Put32(central, 0); Put32(central, 0);
        Put32(central, static_cast<unsigned>(data.size())); Put32(central, static_cast<unsigned>(data.size()));
        Put16(central, static_cast<unsigned>(name.size())); Put16(central, 0); Put16(central, 0);
        Put16(central, 0); Put16(central, 0); Put32(central, 0); Put32(central, local);
        central.insert(central.end(), name.begin(), name.end());
    }
    const unsigned centralAt = static_cast<unsigned>(zip.size());
    zip.insert(zip.end(), central.begin(), central.end());
    Put32(zip, 0x06054b50);
    Put16(zip, 0); Put16(zip, 0);
    Put16(zip, static_cast<unsigned>(files.size())); Put16(zip, static_cast<unsigned>(files.size()));
    Put32(zip, static_cast<unsigned>(central.size())); Put32(zip, centralAt); Put16(zip, 0);
    return zip;
}

void TestZip(const char* realArchive)
{
    std::vector<ZipFile> files;
    const std::vector<unsigned char> zip =
        MakeZip({{"dinput8.dll", "LOADER"}, {"Timestone.asi", "PLUGIN-BYTES"}});
    Check(ListStoredZip(zip, files) && files.size() == 2, "two stored files listed");
    Check(files.size() == 2 && files[1].name == "Timestone.asi" && files[1].size == 12 &&
              memcmp(zip.data() + files[1].offset, "PLUGIN-BYTES", 12) == 0,
          "file bytes located");
    Check(ListStoredZip(MakeZip({{"dinput8.dll", "L"}, {"Timestone.asi", "P"},
                  {"TimestoneInstall.exe", "I"}, {"Timestone-THIRDPARTYNOTICES", "N"}}), files),
          "full package listed");
    DropPackageOnlyFiles(files);
    Check(files.size() == 2 && files[0].name == "dinput8.dll" && files[1].name == "Timestone.asi",
          "installer and notices stay out of the game folder");
    Check(!ListStoredZip(MakeZip({{"../evil.dll", "x"}}), files), "path outside the folder refused");
    Check(!ListStoredZip(MakeZip({{"sub/a.dll", "x"}}), files), "subfolder refused");
    Check(!ListStoredZip(MakeZip({{"a.dll", "x"}, {"a.dll", "y"}}), files), "duplicate refused");
    Check(!ListStoredZip(MakeZip({{"a.dll", "x"}}, 8), files), "compressed file refused");
    std::vector<unsigned char> cut = zip;
    cut.resize(cut.size() - 30);
    Check(!ListStoredZip(cut, files), "truncated archive refused");

    if (FILE* file = fopen(realArchive, "rb")) {
        std::vector<unsigned char> real;
        unsigned char buffer[65536];
        for (size_t got; (got = fread(buffer, 1, sizeof(buffer), file)) > 0;)
            real.insert(real.end(), buffer, buffer + got);
        fclose(file);
        bool asi = false, loader = false, installer = false;
        const bool listed = ListStoredZip(real, files);
        for (const ZipFile& entry : files) {
            asi |= entry.name == "Timestone.asi" && real[entry.offset] == 'M';
            loader |= entry.name == "dinput8.dll" && real[entry.offset] == 'M';
            installer |= entry.name == "TimestoneInstall.exe" && real[entry.offset] == 'M';
        }
        Check(listed && asi && loader && installer, "the built Timestone.zip lists its three binaries");
        printf("%s: %zu files\n", realArchive, files.size());
    } else {
        printf("%s not found; skipped\n", realArchive);
    }
}

void TestRelease(const char* builtPlugin)
{
    const std::string json =
        "{\"tag_name\":\"v0.6.7\",\"assets\":[{\"name\":\"Timestone.zip\",\"uploader\":{\"login\":\"x\"},"
        "\"size\":7884374,\"digest\":\"sha256:ec2b684db1845063d3e92280fc43126b75047471c165184870be6808e4aa69ef\","
        "\"browser_download_url\":\"https://github.com/tilderain/Timestone/releases/download/v0.6.7/Timestone.zip\"},"
        "{\"name\":\"TimestoneInstall.sh\",\"size\": 12,\"digest\":\"sha256:"
        "0000000000000000000000000000000000000000000000000000000000000000\","
        "\"browser_download_url\":\"https://example.invalid/sh\"}]}";
    std::string tag, url, sha;
    unsigned long long size = 1;
    Check(ExtractJsonString(json, "tag_name", 0, tag) && tag == "v0.6.7", "release tag");
    Check(ParseReleaseAsset(json, "Timestone.zip", url, sha, &size) && size == 7884374 &&
              sha.size() == 64 && sha[0] == 'e' && url.rfind("/Timestone.zip") != std::string::npos,
          "zip asset: url, digest and size");
    Check(ParseReleaseAsset(json, "TimestoneInstall.sh", url, sha, &size) && size == 12 &&
              url == "https://example.invalid/sh",
          "second asset is not confused with the first");
    Check(!ParseReleaseAsset(json, "Missing.zip", url, sha), "missing asset");

    Check(IsNewerVersion("v0.6.7", "0.6.6") && !IsNewerVersion("v0.6.6", "0.6.6") &&
              !IsNewerVersion("0.6.6", "0.7.0") && IsNewerVersion("0.10.0", "0.9.9") &&
              IsNewerVersion(L"v1.0", L"0.9.9") && IsNewerVersion("nightly", "0.6.6") &&
              !IsNewerVersion("nightly", "nightly"),
          "version order");

    Check(Sha256Hex(reinterpret_cast<const unsigned char*>("abc"), 3) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 of abc");
    Check(Sha256Hex(nullptr, 0) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of nothing");
    const std::string longText(1000, 'a'); // crosses blocks, and a 56..63 byte tail (1000 % 64 = 40) is below
    Check(Sha256Hex(reinterpret_cast<const unsigned char*>(longText.data()), 1000) ==
              "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "SHA-256 of a thousand a");
    Check(Sha256Hex(reinterpret_cast<const unsigned char*>(longText.data()), 60) ==
              "11ee391211c6256460b6ed375957fadd8061cafbb31daf967db875aebd5aaad4",
          "SHA-256 with the length in a second block");

    if (FILE* file = fopen(builtPlugin, "rb")) {
        std::vector<unsigned char> bytes;
        unsigned char buffer[65536];
        for (size_t got; (got = fread(buffer, 1, sizeof(buffer), file)) > 0;)
            bytes.insert(bytes.end(), buffer, buffer + got);
        fclose(file);
        const std::string version = PeProductVersion(bytes);
        printf("%s: version %s\n", builtPlugin, version.c_str());
        Check(!version.empty() && version[0] >= '0' && version[0] <= '9', "plugin version read from its file");
    }
}

// Text as boxes: about the width a proportional font gives, wrapped the way
// the real painter wraps.
class BoxText final : public TextPainter {
public:
    explicit BoxText(SoftCanvas& canvas) : canvas_(canvas) {}

    float BlockHeight(FontId font, const wchar_t* text, float width, unsigned) override
    {
        return Lines(font, text, width) * LineHeight(font);
    }

    void Draw(FontId font, const wchar_t* text, Rect rect, Color color, unsigned flags) override
    {
        if (!text[0]) return;
        const float line = LineHeight(font), ink = kFontHeight[static_cast<int>(font)] * 0.62f;
        const bool single = flags & (kTextMiddle | kTextRight);
        const int lines = single ? 1 : Lines(font, text, rect.w);
        float width = Width(font, text);
        if (width > rect.w) width = rect.w;
        for (int i = 0; i < lines; ++i) {
            const float lineW = lines > 1 && i + 1 < lines ? rect.w * 0.94f
                                : lines > 1 ? rect.w * 0.5f : width;
            const float y = single ? rect.y + (rect.h - ink) * 0.5f : rect.y + i * line + (line - ink) * 0.5f;
            const float x = flags & kTextRight ? rect.x + rect.w - lineW : rect.x + (rect.w - lineW) * 0.5f;
            canvas_.Fill(x, y, lineW, ink, lobby::WithAlpha(color, 0.55f));
        }
    }

private:
    static float LineHeight(FontId font) { return kFontHeight[static_cast<int>(font)] * 1.3f; }
    static float Width(FontId font, const wchar_t* text)
    {
        return wcslen(text) * kFontHeight[static_cast<int>(font)] * 0.5f;
    }
    static int Lines(FontId font, const wchar_t* text, float width)
    {
        int lines = 0;
        const wchar_t* start = text;
        for (const wchar_t* c = text;; ++c) {
            if (*c && *c != L'\n') continue;
            const float w = (c - start) * kFontHeight[static_cast<int>(font)] * 0.5f;
            lines += w > width ? static_cast<int>(w / width) + 1 : 1;
            if (!*c) break;
            start = c + 1;
        }
        return lines;
    }

    SoftCanvas& canvas_;
};

void RenderScreens(const char* directory)
{
    std::vector<unsigned char> tile(lobby::kSpaceStarsTileBytes);
    std::vector<uint8_t> stars(lobby::kSpaceStarsRgbaBytes);
    FILE* file = fopen("UMVC3Hook/assets/space_stars_512.dxt1", "rb");
    const bool haveStars = file && fread(tile.data(), 1, tile.size(), file) == tile.size() &&
                           lobby::BuildSpaceStarsRgba(tile.data(), tile.size(), stars.data());
    if (file) fclose(file);
    Check(haveStars, "star tile read (run from the repository root)");

    struct Shot {
        const char* name;
        View view;
        uint32_t frame;
    };
    std::vector<Shot> shots;
    {
        View v;
        v.title = L"TIMESTONE";
        v.message = L"Looking for Ultimate Marvel vs. Capcom 3...";
        v.busy = true;
        shots.push_back({"searching", v, 40});
    }
    {
        View v;
        v.title = L"INSTALL TIMESTONE";
        v.message = L"Timestone v0.6.6 will be installed into:";
        v.detail = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\ULTIMATE MARVEL VS. CAPCOM 3";
        v.buttons[0] = L"INSTALL"; v.buttons[1] = L"BROWSE"; v.buttons[2] = L"CLOSE";
        v.buttonCount = 3;
        shots.push_back({"ready", v, 1500});
    }
    {
        View v;
        v.title = L"TIMESTONE INSTALL";
        v.message = L"Downloading Timestone v0.6.6";
        v.progress = true;
        v.fraction = 0.62f;
        v.status = L"4.9 MB of 7.9 MB";
        shots.push_back({"progress", v, 3200});
    }
    {
        View v;
        v.title = L"INSTALL FAILED";
        v.message = L"The new files could not be put into the game folder.\n"
                    L"Close the game and anything else using the folder, then try again.\n\n"
                    L"Windows error 5 on dinput8.dll";
        v.buttons[0] = L"RETRY"; v.buttons[1] = L"CLOSE";
        v.buttonCount = 2;
        v.cursor = 1;
        shots.push_back({"failed", v, 4700});
    }

    const int width = 960, height = 540;
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    for (Shot& shot : shots) {
        shot.view.footer = L"Timestone Install 0.6.6";
        SoftCanvas canvas(pixels.data(), width, height, haveStars ? stars.data() : nullptr);
        BoxText text(canvas);
        const Layout layout = DrawScreen(canvas, text, shot.view, shot.frame);
        Check(layout.panel.y > 0.0f && layout.panel.y + layout.panel.h < kScreenH, "panel fits the window");
        for (int i = 0; i < shot.view.buttonCount; ++i)
            Check(layout.buttons[i].x >= layout.panel.x + 20.0f &&
                      layout.buttons[i].x + layout.buttons[i].w <= layout.panel.x + layout.panel.w - 20.0f,
                  "buttons inside the panel");
        const std::string path = std::string(directory) + "/install_" + shot.name + ".ppm";
        FILE* out = fopen(path.c_str(), "wb");
        if (!out) {
            Check(false, "picture written");
            continue;
        }
        fprintf(out, "P6\n%d %d\n255\n", width, height);
        for (const uint32_t pixel : pixels) {
            const unsigned char rgb[3] = {static_cast<unsigned char>(pixel >> 16),
                                          static_cast<unsigned char>(pixel >> 8),
                                          static_cast<unsigned char>(pixel)};
            fwrite(rgb, 1, 3, out);
        }
        fclose(out);
        printf("wrote %s\n", path.c_str());
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestVdf();
    TestOverride();
    TestZip("Timestone.zip");
    TestRelease("Timestone.asi");
    if (argc > 1) RenderScreens(argv[1]);
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("install checks passed\n");
    return 0;
}
