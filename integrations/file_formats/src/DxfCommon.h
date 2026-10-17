#pragma once
// SPDX-License-Identifier: Apache-2.0
// Internal helpers shared by the DXF reader and writer.

#include "openmine/formats/Dxf.h"
#include "openmine/geometry/Transform.h"

#include <string>
#include <string_view>
#include <vector>

namespace openmine::formats::dxf {

struct Group {
    int code = 0;
    std::string value;
    double num() const;
    int integer() const;
};

/// Splits an ASCII DXF into group code / value pairs. Throws DxfError on malformed input.
std::vector<Group> tokenize(std::string_view text);

/// Arbitrary-axis algorithm: OCS X axis for extrusion direction N.
Vec3 ocsXAxis(Vec3 n);
/// OCS → WCS transform for extrusion direction N.
Transform3 ocsToWcs(Vec3 n);
/// WCS → OCS transform for extrusion direction N.
Transform3 wcsToOcs(Vec3 n);

/// Decodes \U+XXXX escapes; converts Windows-1252 bytes to UTF-8 when `cp1252` and not valid UTF-8.
std::string decodeText(std::string_view s, bool cp1252);
/// Encodes non-ASCII code points as \U+XXXX (R2004 files are not UTF-8).
std::string encodeText(std::string_view utf8);
/// Removes MTEXT inline formatting (\P → newline, {\f...;} groups, \S stacking, etc.).
std::string stripMText(std::string_view s);

/// Shortest round-trip decimal representation, always with a decimal point or exponent.
std::string formatReal(double v);

/// Simple writer of group code / value pairs.
class Out {
public:
    void g(int code, std::string_view v);
    void g(int code, const char* v) { g(code, std::string_view(v)); }
    void g(int code, const std::string& v) { g(code, std::string_view(v)); }
    void g(int code, double v);
    void g(int code, int v);
    void g(int code, unsigned v) { g(code, static_cast<int>(v)); }
    void g(int code, long v) { g(code, static_cast<int>(v)); }
    void point(int code, Vec3 p, bool with3d = true);
    void handle(int code, unsigned h);
    std::string& str() { return buf_; }

private:
    std::string buf_;
};

std::string hex(unsigned h);

} // namespace openmine::formats::dxf
