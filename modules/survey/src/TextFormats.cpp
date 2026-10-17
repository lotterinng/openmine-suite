// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/TextFormats.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace openmine::survey {

namespace {

/// Buffered line reader with byte-position progress.
class LineReader {
public:
    explicit LineReader(const std::filesystem::path& f) : path_(f) {
        in_.open(f, std::ios::binary);
        if (!in_) throw PointCloudError("Cannot open " + f.string());
        size_ = std::filesystem::file_size(f);
    }
    bool next(std::string_view& line) {
        while (true) {
            const auto nl = buf_.find('\n', pos_);
            if (nl != std::string::npos) {
                line = std::string_view(buf_).substr(pos_, nl - pos_);
                consumed_ += nl + 1 - pos_;
                pos_ = nl + 1;
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                ++lineNo_;
                return true;
            }
            if (eof_) {
                if (pos_ >= buf_.size()) return false;
                line = std::string_view(buf_).substr(pos_);
                consumed_ += buf_.size() - pos_;
                pos_ = buf_.size();
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                ++lineNo_;
                return true;
            }
            buf_.erase(0, pos_);
            pos_ = 0;
            const std::size_t old = buf_.size();
            buf_.resize(old + (1 << 20));
            in_.read(buf_.data() + old, 1 << 20);
            buf_.resize(old + static_cast<std::size_t>(in_.gcount()));
            if (in_.gcount() == 0) eof_ = true;
        }
    }
    void rewind() {
        in_.clear();
        in_.seekg(0);
        buf_.clear();
        pos_ = 0;
        eof_ = false;
        consumed_ = 0;
        lineNo_ = 0;
    }
    double progress() const { return size_ ? static_cast<double>(consumed_) / static_cast<double>(size_) : 1.0; }
    std::size_t lineNo() const { return lineNo_; }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::ifstream in_;
    std::string buf_;
    std::size_t pos_ = 0, consumed_ = 0, size_ = 0, lineNo_ = 0;
    bool eof_ = false;
};

void split(std::string_view line, char delim, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t i = 0;
    if (delim == ' ') { // any run of whitespace
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            const std::size_t b = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            if (i > b) out.push_back(line.substr(b, i - b));
        }
        return;
    }
    while (true) {
        const auto d = line.find(delim, i);
        std::string_view f = line.substr(i, d == std::string_view::npos ? std::string_view::npos : d - i);
        while (!f.empty() && (f.front() == ' ' || f.front() == '\t' || f.front() == '"')) f.remove_prefix(1);
        while (!f.empty() && (f.back() == ' ' || f.back() == '\t' || f.back() == '"')) f.remove_suffix(1);
        out.push_back(f);
        if (d == std::string_view::npos) break;
        i = d + 1;
    }
}

bool parseDouble(std::string_view s, double& v) {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool isComment(std::string_view l) {
    std::size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    return i >= l.size() || l[i] == '#' || l.substr(i, 2) == "//";
}

char detectDelimiter(std::string_view line) {
    for (char d : {',', ';', '\t'})
        if (line.find(d) != std::string_view::npos) return d;
    return ' ';
}

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

TextColumn roleFromName(const std::string& n) {
    static const std::vector<std::pair<std::vector<std::string>, TextColumn>> names = {
        {{"x", "e", "east", "easting", "y_lo", "lo_y"}, TextColumn::X},
        {{"y", "n", "north", "northing"}, TextColumn::Y},
        {{"z", "rl", "elev", "elevation", "height", "h", "level"}, TextColumn::Z},
        {{"i", "int", "intensity", "reflectance", "amplitude"}, TextColumn::Intensity},
        {{"r", "red"}, TextColumn::Red},
        {{"g", "green"}, TextColumn::Green},
        {{"b", "blue"}, TextColumn::Blue},
        {{"class", "classification", "cls"}, TextColumn::Classification},
        {{"nx", "normalx", "normal_x"}, TextColumn::NormalX},
        {{"ny", "normaly", "normal_y"}, TextColumn::NormalY},
        {{"nz", "normalz", "normal_z"}, TextColumn::NormalZ},
        {{"t", "time", "gpstime", "gps_time"}, TextColumn::GpsTime},
    };
    for (const auto& [list, role] : names)
        if (std::find(list.begin(), list.end(), n) != list.end()) return role;
    return TextColumn::Scalar;
}

std::uint16_t scale16(double v, double max) {
    if (max <= 0) max = 65535;
    return static_cast<std::uint16_t>(std::clamp(std::lround(v / max * 65535.0), 0L, 65535L));
}

/// Shared parser for delimited text and PTS records.
class DelimitedSource : public PointSource {
public:
    DelimitedSource(const std::filesystem::path& f, TextReadOptions o, std::string format, bool pts)
        : r_(f), opt_(std::move(o)), format_(std::move(format)), pts_(pts) {
        analyse();
    }
    PointSchema schema() const override { return schema_; }
    std::string formatName() const override { return format_; }
    std::optional<std::int64_t> countHint() const override { return countHint_; }
    double progress() const override { return r_.progress(); }

    void rewind() override {
        r_.rewind();
        skipPreamble();
    }

    std::size_t read(PointBuffer& out, std::size_t max) override {
        if (out.empty()) out.setSchema(schema_);
        std::string_view line;
        std::size_t n = 0;
        std::vector<std::string_view> f;
        std::vector<double> v;
        while (n < max && r_.next(line)) {
            if (isComment(line)) continue;
            split(line, delim_, f);
            if (pts_ && f.size() == 1) continue; // scan header (point count)
            if (f.size() < minFields_) {
                if (strict_) throw PointCloudError(r_.path().filename().string() + " line " + std::to_string(r_.lineNo()) + ": expected " +
                                                  std::to_string(minFields_) + " values");
                continue;
            }
            v.assign(f.size(), 0.0);
            bool ok = true;
            for (std::size_t k = 0; k < f.size() && k < roles_.size(); ++k)
                if (roles_[k] != TextColumn::Ignore && !parseDouble(f[k], v[k])) ok = false;
            if (!ok) throw PointCloudError(r_.path().filename().string() + " line " + std::to_string(r_.lineNo()) + ": invalid number");
            Vec3 p;
            std::array<float, 3> nrm{0, 0, 0};
            Rgb16 c;
            double in = 0, cls = 0, t = 0;
            std::size_t si = 0;
            for (std::size_t k = 0; k < roles_.size() && k < v.size(); ++k) switch (roles_[k]) {
                case TextColumn::X: p.x = v[k]; break;
                case TextColumn::Y: p.y = v[k]; break;
                case TextColumn::Z: p.z = v[k]; break;
                case TextColumn::Intensity: in = v[k]; break;
                case TextColumn::Red: c.r = scale16(v[k], colorMax_); break;
                case TextColumn::Green: c.g = scale16(v[k], colorMax_); break;
                case TextColumn::Blue: c.b = scale16(v[k], colorMax_); break;
                case TextColumn::Classification: cls = v[k]; break;
                case TextColumn::NormalX: nrm[0] = static_cast<float>(v[k]); break;
                case TextColumn::NormalY: nrm[1] = static_cast<float>(v[k]); break;
                case TextColumn::NormalZ: nrm[2] = static_cast<float>(v[k]); break;
                case TextColumn::GpsTime: t = v[k]; break;
                case TextColumn::Scalar: out.scalars[si++].push_back(static_cast<float>(v[k])); break;
                case TextColumn::Ignore: break;
                }
            out.xyz.push_back(p);
            if (schema_.intensity)
                out.intensity.push_back(pts_ ? static_cast<std::uint16_t>(std::clamp(std::lround((in + 2048.0) / 4095.0 * 65535.0), 0L, 65535L)) : scale16(in - intensityMin_, intensityMax_));
            if (schema_.rgb) out.rgb.push_back(c);
            if (schema_.classification) out.classification.push_back(static_cast<std::uint8_t>(std::clamp(cls, 0.0, 255.0)));
            if (schema_.normals) out.normal.push_back(nrm);
            if (schema_.gpsTime) out.gpsTime.push_back(t);
            ++n;
        }
        return n;
    }

private:
    LineReader r_;
    TextReadOptions opt_;
    std::string format_;
    bool pts_ = false;
    bool strict_ = true;
    char delim_ = ' ';
    bool hasHeader_ = false;
    std::vector<TextColumn> roles_;
    std::size_t minFields_ = 3;
    PointSchema schema_;
    double colorMax_ = 255, intensityMax_ = 65535, intensityMin_ = 0;
    std::optional<std::int64_t> countHint_;

    void skipPreamble() {
        std::string_view line;
        for (int k = 0; k < opt_.skipLines; ++k) r_.next(line);
        if (hasHeader_) r_.next(line);
    }

    void analyse() {
        std::string_view line;
        for (int k = 0; k < opt_.skipLines; ++k)
            if (!r_.next(line)) throw PointCloudError("The file is shorter than the lines to skip");
        // First non-comment line.
        std::string first;
        while (r_.next(line)) {
            if (isComment(line)) continue;
            first = std::string(line);
            break;
        }
        if (first.empty()) throw PointCloudError("The file contains no points");
        delim_ = opt_.delimiter ? opt_.delimiter : detectDelimiter(first);
        std::vector<std::string_view> f;
        split(first, delim_, f);
        if (pts_ && f.size() == 1) { // PTS count line: take the next line as the first record
            double c = 0;
            if (parseDouble(f[0], c)) countHint_ = static_cast<std::int64_t>(c);
            while (r_.next(line))
                if (!isComment(line)) {
                    first = std::string(line);
                    break;
                }
            split(first, delim_, f);
        }
        bool numeric = true;
        for (auto s : f) {
            double d;
            numeric = numeric && parseDouble(s, d);
        }
        hasHeader_ = opt_.header == 1 || (opt_.header == -1 && !numeric);
        std::vector<std::string> names;
        if (hasHeader_)
            for (auto s : f) names.push_back(lower(s));
        // Roles.
        if (!opt_.columns.empty()) roles_ = opt_.columns;
        else if (hasHeader_) {
            for (const auto& n : names) roles_.push_back(roleFromName(n));
        } else {
            const std::size_t n = f.size();
            if (n < 3) throw PointCloudError("Cannot identify X, Y, Z columns: fewer than three values per line");
            roles_ = {TextColumn::X, TextColumn::Y, TextColumn::Z};
            if (n == 4 || n == 7) roles_.push_back(TextColumn::Intensity);
            if (n == 6 || n == 7) {
                roles_.push_back(TextColumn::Red);
                roles_.push_back(TextColumn::Green);
                roles_.push_back(TextColumn::Blue);
            }
            while (roles_.size() < n) roles_.push_back(TextColumn::Scalar);
        }
        int nx = 0, ny = 0, nz = 0;
        for (auto r : roles_) {
            nx += r == TextColumn::X;
            ny += r == TextColumn::Y;
            nz += r == TextColumn::Z;
        }
        if (nx != 1 || ny != 1 || nz != 1) throw PointCloudError("Exactly one X, Y and Z column is required (found " + std::to_string(nx) + "/" + std::to_string(ny) + "/" + std::to_string(nz) + ")");
        minFields_ = 0;
        for (std::size_t k = 0; k < roles_.size(); ++k)
            if (roles_[k] != TextColumn::Ignore) minFields_ = k + 1;
        for (std::size_t k = 0; k < roles_.size(); ++k) {
            switch (roles_[k]) {
            case TextColumn::Intensity: schema_.intensity = true; break;
            case TextColumn::Red: case TextColumn::Green: case TextColumn::Blue: schema_.rgb = true; break;
            case TextColumn::Classification: schema_.classification = true; break;
            case TextColumn::NormalX: case TextColumn::NormalY: case TextColumn::NormalZ: schema_.normals = true; break;
            case TextColumn::GpsTime: schema_.gpsTime = true; break;
            case TextColumn::Scalar: schema_.scalarFields.push_back(k < names.size() && !names[k].empty() ? names[k] : "field" + std::to_string(k + 1)); break;
            default: break;
            }
        }
        // Value ranges for intensity / colour scaling: sample the first 10 000 records.
        r_.rewind();
        skipPreamble();
        double imax = 0, imin = 0, cmax = 0;
        int seen = 0;
        std::vector<double> v;
        while (seen < 10000 && r_.next(line)) {
            if (isComment(line)) continue;
            split(line, delim_, f);
            if (pts_ && f.size() == 1) continue;
            for (std::size_t k = 0; k < f.size() && k < roles_.size(); ++k) {
                double d = 0;
                if (!parseDouble(f[k], d)) continue;
                if (roles_[k] == TextColumn::Intensity) {
                    imax = std::max(imax, d);
                    imin = std::min(imin, d);
                }
                if (roles_[k] == TextColumn::Red || roles_[k] == TextColumn::Green || roles_[k] == TextColumn::Blue) cmax = std::max(cmax, d);
            }
            ++seen;
        }
        colorMax_ = opt_.colorMax > 0 ? opt_.colorMax : (cmax <= 1.0 ? 1.0 : (cmax <= 255 ? 255.0 : 65535.0));
        if (opt_.intensityMax > 0) intensityMax_ = opt_.intensityMax;
        else if (imin < 0) { // signed intensities (e.g. -2048…2047): shift to zero
            intensityMin_ = imin < -2048 ? imin : -2048;
            intensityMax_ = std::max(4095.0, imax - intensityMin_);
        } else intensityMax_ = imax <= 1.0 ? 1.0 : (imax <= 255 ? 255.0 : 65535.0);
        r_.rewind();
        skipPreamble();
    }
};

/// Leica PTX: per scan "cols\nrows\n" + scanner position + 3 axes + 4×4 matrix (rows), then cols×rows points.
class PtxSource : public PointSource {
public:
    explicit PtxSource(const std::filesystem::path& f) : r_(f) { probe(); }
    PointSchema schema() const override { return schema_; }
    std::string formatName() const override { return "PTX"; }
    std::optional<std::int64_t> countHint() const override { return total_; }
    double progress() const override { return r_.progress(); }
    void rewind() override {
        r_.rewind();
        remaining_ = 0;
    }
    std::vector<Vec3> scanners() const { return scanners_; }

    std::size_t read(PointBuffer& out, std::size_t max) override {
        if (out.empty()) out.setSchema(schema_);
        std::size_t n = 0;
        std::string_view line;
        std::vector<std::string_view> f;
        while (n < max) {
            if (remaining_ == 0 && !readHeader(m_)) break;
            if (!r_.next(line)) throw PointCloudError("PTX file ends inside a scan");
            --remaining_;
            split(line, ' ', f);
            if (f.size() < 3) throw PointCloudError("PTX line " + std::to_string(r_.lineNo()) + ": expected x y z");
            double v[7] = {0, 0, 0, 0, 0, 0, 0};
            for (std::size_t k = 0; k < f.size() && k < 7; ++k)
                if (!parseDouble(f[k], v[k])) throw PointCloudError("PTX line " + std::to_string(r_.lineNo()) + ": invalid number");
            if (v[0] == 0 && v[1] == 0 && v[2] == 0) continue; // no return
            out.xyz.push_back(m_.apply({v[0], v[1], v[2]}));
            if (schema_.intensity) out.intensity.push_back(static_cast<std::uint16_t>(std::clamp(std::lround(v[3] * 65535.0), 0L, 65535L)));
            if (schema_.rgb) out.rgb.push_back({static_cast<std::uint16_t>(v[4] * 257), static_cast<std::uint16_t>(v[5] * 257), static_cast<std::uint16_t>(v[6] * 257)});
            ++n;
        }
        return n;
    }

private:
    LineReader r_;
    PointSchema schema_;
    std::int64_t total_ = 0, remaining_ = 0;
    Transform3 m_;
    std::vector<Vec3> scanners_;

    bool readHeader(Transform3& m, Vec3* scanner = nullptr) {
        std::string_view line;
        std::vector<std::string_view> f;
        auto nums = [&](int count) {
            std::vector<double> v;
            if (!r_.next(line)) throw PointCloudError("Truncated PTX header");
            split(line, ' ', f);
            if (static_cast<int>(f.size()) < count) throw PointCloudError("PTX header line " + std::to_string(r_.lineNo()) + " is malformed");
            for (int k = 0; k < count; ++k) {
                double d;
                if (!parseDouble(f[static_cast<std::size_t>(k)], d)) throw PointCloudError("PTX header line " + std::to_string(r_.lineNo()) + " is malformed");
                v.push_back(d);
            }
            return v;
        };
        // Skip blank lines between scans.
        do {
            if (!r_.next(line)) return false;
        } while (isComment(line));
        split(line, ' ', f);
        double cols = 0, rows = 0;
        if (f.size() != 1 || !parseDouble(f[0], cols)) throw PointCloudError("PTX: expected the column count on line " + std::to_string(r_.lineNo()));
        rows = nums(1)[0];
        const auto pos = nums(3);
        nums(3);
        nums(3);
        nums(3);
        std::array<double, 16> mm{};
        for (int rr = 0; rr < 4; ++rr) {
            const auto row = nums(4);
            for (int c = 0; c < 4; ++c) mm[static_cast<std::size_t>(c * 4 + rr)] = row[static_cast<std::size_t>(c)]; // file rows are matrix columns
        }
        m = Transform3(mm);
        remaining_ = static_cast<std::int64_t>(cols) * static_cast<std::int64_t>(rows);
        if (scanner) *scanner = {pos[0], pos[1], pos[2]}; // "scanner registered position" (already in world coordinates)
        return true;
    }

    void probe() {
        // Count points and detect columns from the first data line of each scan.
        Transform3 m;
        Vec3 sc;
        std::string_view line;
        std::vector<std::string_view> f;
        bool first = true;
        while (readHeader(m, &sc)) {
            scanners_.push_back(sc);
            total_ += remaining_;
            if (first && remaining_ > 0 && r_.next(line)) {
                split(line, ' ', f);
                schema_.intensity = f.size() >= 4;
                schema_.rgb = f.size() >= 7;
                --remaining_;
                first = false;
            }
            while (remaining_ > 0 && r_.next(line)) --remaining_;
        }
        if (scanners_.empty()) throw PointCloudError("The PTX file contains no scans");
        rewind();
    }
};

std::string fmt(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

class TextSink : public PointSink {
public:
    TextSink(const std::filesystem::path& f, TextWriteOptions o, bool pts) : path_(f), opt_(o), pts_(pts) {}
    void begin(const PointSchema& s) override {
        schema_ = s;
        out_.open(path_, std::ios::binary | std::ios::trunc);
        if (!out_) throw PointCloudError("Cannot write " + path_.string());
        if (pts_) {
            countPos_ = out_.tellp();
            out_ << std::string(20, ' ') << "\n"; // count placeholder, patched in finish()
            return;
        }
        if (!opt_.header) return;
        const char d = opt_.delimiter;
        out_ << "x" << d << "y" << d << "z";
        if (s.intensity) out_ << d << "intensity";
        if (s.rgb) out_ << d << "red" << d << "green" << d << "blue";
        if (s.classification) out_ << d << "classification";
        if (s.normals) out_ << d << "nx" << d << "ny" << d << "nz";
        if (s.gpsTime) out_ << d << "gps_time";
        for (const auto& n : s.scalarFields) out_ << d << n;
        out_ << "\n";
    }
    void write(const PointBuffer& b) override {
        std::string line;
        const char d = pts_ ? ' ' : opt_.delimiter;
        const int dec = pts_ ? 4 : opt_.decimals;
        for (std::size_t i = 0; i < b.size(); ++i) {
            line.clear();
            line += fmt(b.xyz[i].x, dec) + d + fmt(b.xyz[i].y, dec) + d + fmt(b.xyz[i].z, dec);
            if (pts_) {
                const int in = schema_.intensity && b.schema().intensity ? static_cast<int>(std::lround(b.intensity[i] / 65535.0 * 4095.0)) - 2048 : 0;
                line += d + std::to_string(in);
                if (schema_.rgb && b.schema().rgb)
                    line += d + std::to_string(b.rgb[i].r >> 8) + d + std::to_string(b.rgb[i].g >> 8) + d + std::to_string(b.rgb[i].b >> 8);
            } else {
                if (schema_.intensity) line += d + std::to_string(b.schema().intensity ? b.intensity[i] : 0);
                if (schema_.rgb) {
                    const Rgb16 c = b.schema().rgb ? b.rgb[i] : Rgb16{};
                    line += d + std::to_string(c.r) + d + std::to_string(c.g) + d + std::to_string(c.b);
                }
                if (schema_.classification) line += d + std::to_string(b.schema().classification ? b.classification[i] : 0);
                if (schema_.normals) {
                    const auto n = b.schema().normals ? b.normal[i] : std::array<float, 3>{0, 0, 0};
                    line += d + fmt(n[0], 6) + d + fmt(n[1], 6) + d + fmt(n[2], 6);
                }
                if (schema_.gpsTime) line += d + fmt(b.schema().gpsTime ? b.gpsTime[i] : 0.0, 6);
                for (const auto& name : schema_.scalarFields) {
                    const int k = b.scalarIndex(name);
                    line += d + fmt(k >= 0 ? b.scalars[static_cast<std::size_t>(k)][i] : 0.0f, 6);
                }
            }
            line += '\n';
            out_ << line;
        }
        count_ += b.size();
    }
    void finish() override {
        if (pts_) {
            out_.seekp(countPos_);
            const std::string c = std::to_string(count_);
            out_ << c;
        }
        out_.close();
        if (!out_) throw PointCloudError("Writing " + path_.string() + " failed");
    }

private:
    std::filesystem::path path_;
    TextWriteOptions opt_;
    bool pts_;
    PointSchema schema_;
    std::ofstream out_;
    std::streampos countPos_{};
    std::size_t count_ = 0;
};

} // namespace

std::unique_ptr<PointSource> openTextSource(const std::filesystem::path& file, const TextReadOptions& o) {
    std::string ext = lower(file.extension().string());
    std::string fmtName = ext == ".csv" ? "CSV" : (ext == ".xyz" ? "XYZ" : "TXT");
    return std::make_unique<DelimitedSource>(file, o, fmtName, false);
}

std::unique_ptr<PointSource> openPtsSource(const std::filesystem::path& file) {
    TextReadOptions o;
    o.delimiter = ' ';
    o.header = 0;
    return std::make_unique<DelimitedSource>(file, o, "PTS", true);
}

std::unique_ptr<PointSource> openPtxSource(const std::filesystem::path& file) { return std::make_unique<PtxSource>(file); }

std::vector<Vec3> ptxScannerPositions(const std::filesystem::path& file) { return PtxSource(file).scanners(); }

std::unique_ptr<PointSink> textSink(const std::filesystem::path& file, const TextWriteOptions& o) {
    return std::make_unique<TextSink>(file, o, false);
}

std::unique_ptr<PointSink> ptsSink(const std::filesystem::path& file) { return std::make_unique<TextSink>(file, TextWriteOptions{}, true); }

} // namespace openmine::survey
