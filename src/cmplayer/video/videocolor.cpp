#include "videocolor.hpp"
#include "enum/colorrange.hpp"
#include "misc/json.hpp"
#include "misc/log.hpp"

extern "C" {
extern const char *const mp_csp_names[MP_CSP_COUNT];
}

DECLARE_LOG_CONTEXT(Video)

static auto makeNameArray() -> VideoColor::Array<QString>
{
    VideoColor::Array<QString> names;
    names[VideoColor::Brightness] = u"brightness"_q;
    names[VideoColor::Contrast]   = u"contrast"_q;
    names[VideoColor::Saturation] = u"saturation"_q;
    names[VideoColor::Hue]        = u"hue"_q;
    return names;
}

VideoColor::Array<QString> VideoColor::s_names = makeNameArray();

struct YCbCrRange {
    auto operator *= (float rhs) -> YCbCrRange&
        { y1 *= rhs; y2 *= rhs;  c1 *= rhs; c2 *= rhs; return *this; }
    auto operator * (float rhs) const -> YCbCrRange
        { return YCbCrRange(*this) *= rhs; }
    float y1, y2, c1, c2;
    auto base() const -> QVector3D
        { return { y1, (c1 + c2)*.5f, (c1 + c2)*.5f }; }
};

//  y1,y2,c1,c2
const YCbCrRange ranges[5] = {
{  0.f/255.f, 255.f/255.f,  0.f/255.f, 255.f/255.f}, // Auto
{ 16.f/255.f, 235.f/255.f, 16.f/255.f, 240.f/255.f}, // Limited
{  0.f/255.f, 255.f/255.f,  0.f/255.f, 255.f/255.f}, // Full
{  0.f/255.f, 255.f/255.f,  0.f/255.f, 255.f/255.f}, // Remap
{  0.f/255.f, 255.f/255.f,  0.f/255.f, 255.f/255.f}  // Extended
};

const float specs[MP_CSP_COUNT][2] = {
{0.0000, 0.0000}, // MP_CSP_AUTO,
{0.1140, 0.2990}, // MP_CSP_BT_601,
{0.0722, 0.2126}, // MP_CSP_BT_709,
{0.0870, 0.2120}  // MP_CSP_SMPTE_240M,
};

auto VideoColor::matYCbCrToRgb(mp_csp c, ColorRange r) const -> QMatrix4x4
{
    QMatrix4x4 scaler;
    if (_IsOneOf(r, ColorRange::Remap, ColorRange::Extended)) {
        const auto &tv = ranges[(int)ColorRange::Limited];
        const auto &pc = ranges[(int)ColorRange::Full];
        QVector3D mul;
        if (r == ColorRange::Remap) {
            mul = { (pc.y2 - pc.y1) / (tv.y2 - tv.y1),
                    (pc.c2 - pc.c1) / (tv.c2 - tv.c1),
                    (pc.c2 - pc.c1) / (tv.c2 - tv.c1) };
            scaler(0, 3) = pc.y1 - tv.y1*mul.x();
            scaler(1, 3) = pc.c1 - tv.c1*mul.y();
            scaler(2, 3) = pc.c1 - tv.c1*mul.z();
        } else {
            Q_ASSERT(r == ColorRange::Extended);
            const auto v = (pc.y2 - pc.y1) / (tv.y2 - tv.y1);
            mul = { v, v, v };
            scaler(0, 3) = pc.y1 - tv.y1*mul.x();
            scaler(1, 3) = pc.y1 - tv.y1*mul.y();
            scaler(2, 3) = pc.y1 - tv.y1*mul.z();
        }
        scaler(0, 0) = mul.x();
        scaler(1, 1) = mul.y();
        scaler(2, 2) = mul.z();
    }

    const auto kb = specs[c][0], kr = specs[c][1];
    const auto &range = ranges[(int)r];

    const auto dy = 1.f/(range.y2-range.y1);
    const auto dc = 2.f/(range.c2-range.c1);
    const auto kg = 1.f - kb - kr;

    QMatrix4x4 coef;
    const auto m11 = -dc * (1.f - kb) * kb / kg;
    const auto m12 = -dc * (1.f - kr) * kr / kg;
    coef(0, 0) = dy; coef(0, 1) = 0.0;           coef(0, 2) = (1.0 - kr)*dc;
    coef(1, 0) = dy; coef(1, 1) = m11;           coef(1, 2) = m12;
    coef(2, 0) = dy; coef(2, 1) = dc * (1 - kb); coef(2, 2) = 0.0;

    QMatrix4x4 baseSub;
    baseSub.setColumn(3, { -range.base(), 1.0 });

    QMatrix4x4 filter = matBSHC();

    return coef*filter*baseSub*scaler;
}

SIA matRgbToYCbCr(mp_csp c = MP_CSP_BT_601,
                  ColorRange r = ColorRange::Limited) -> QMatrix4x4
{
    const auto kb = specs[c][0], kr = specs[c][1];
    const auto &range = ranges[(int)r];
    const auto dy = (range.y2-range.y1);
    const auto dc = (range.c2-range.c1)/2.0f;
    const auto kg = 1.0f - kb - kr;

    const double m10 = -dc*kr/(1.0 - kb);
    const double m22 = -dc*kb/(1.0 - kr);
    QMatrix4x4 mat;
    mat(0, 0) = dy * kr; mat(0, 1) = dy * kg;         mat(0, 2) = dy * kb;
    mat(1, 0) = m10;     mat(1, 1) = -dc*kg/(1.0-kb); mat(1, 2) = dc;
    mat(2, 0) = dc;      mat(2, 1) = -dc*kg/(1.0-kr); mat(2, 2) = m22;

    QMatrix4x4 add;
    add.setColumn(3, { range.base(), 1.0 });

    return add*mat;
}

static auto matYCgCoToRgb() -> QMatrix4x4
{
    QMatrix4x4 mat;
    mat(0, 0) = 1; mat(0, 1) = -1; mat(0, 2) =  1;
    mat(1, 0) = 1; mat(1, 1) =  1; mat(1, 2) =  0;
    mat(2, 0) = 1; mat(2, 1) = -1; mat(2, 2) = -1;

    QMatrix4x4 sub;
    sub.setColumn(3, { 0.f, -0.5f, -0.5f, 1.f });
    return mat*sub;
}

auto VideoColor::matBSHC() const -> QMatrix4x4
{
    const auto b = qBound(-1.0, brightness() * 1e-2, 1.0);
    const auto s = qBound(0.0, saturation() * 1e-2 + 1.0, 2.0);
    const auto c = qBound(0.0, contrast() * 1e-2 + 1.0, 2.0);
    const auto h = qBound(-M_PI, hue() * 1e-2 * M_PI, M_PI);
    QMatrix4x4 mat;
    mat(0, 0) =   c; mat(0, 1) = 0.0;          mat(0, 2) = 0.0;
    mat(1, 0) = 0.0; mat(1, 1) =  c*s*qCos(h); mat(1, 2) = c*s*qSin(h);
    mat(2, 0) = 0.0; mat(2, 1) = -c*s*qSin(h); mat(2, 2) = c*s*qCos(h);
    mat(0, 3) = b;
    return mat;
}

auto VideoColor::matrix(mp_csp csp, ColorRange cr) const -> QMatrix4x4
{
    switch (csp) {
    case MP_CSP_BT_601: case MP_CSP_BT_709: case MP_CSP_SMPTE_240M:
        return matYCbCrToRgb(csp, cr);
    case MP_CSP_RGB:    case MP_CSP_YCGCO:
        break;
    default:
        _Warn("Color space %% is not supported", mp_csp_names[csp]);
        return QMatrix4x4();
    }

    if (isZero()) {
        switch (csp) {
        case MP_CSP_RGB:
            return QMatrix4x4();
        case MP_CSP_YCGCO:
            return matYCgCoToRgb();
        default:
            break;
        }
    }

    const auto rgbFromYCbCr = matYCbCrToRgb(MP_CSP_BT_601, ColorRange::Full);
    auto toYCbCr = matRgbToYCbCr(MP_CSP_BT_601, ColorRange::Full);
    if (csp == MP_CSP_YCGCO)
        toYCbCr = toYCbCr*matYCgCoToRgb();
    return rgbFromYCbCr*toYCbCr;
}

auto VideoColor::packed() const -> qint64
{
#define PACK(p, s) ((0xffu & ((quint32)p() + 100u)) << s)
    return PACK(brightness, 24) | PACK(contrast, 16)
           | PACK(saturation, 8) | PACK(hue, 0);
#undef PACK
}

auto VideoColor::fromPacked(qint64 packed) -> VideoColor
{
#define UNPACK(s) (((packed >> s) & 0xff) - 100)
    return VideoColor(UNPACK(24), UNPACK(16), UNPACK(8), UNPACK(0));
#undef UNPACK
}

auto VideoColor::formatText(Type type) -> QString
{
    switch (type) {
    case Brightness:
        return tr("Brightness %1%");
    case Saturation:
        return tr("Saturation %1%");
    case Contrast:
        return tr("Contrast %1%");
    case Hue:
        return tr("Hue %1%");
    default:
        return tr("Reset");
    }
}

auto VideoColor::getText(Type type) const -> QString
{
    const auto value = 0 <= type && type < TypeMax ? _NS(m[type]) : QString();
    const auto format = formatText(type);
    return value.isEmpty() ? format : format.arg(value);
}

auto VideoColor::toString() const -> QString
{
    QStringList strs;
    VideoColor::for_type([&] (VideoColor::Type type) {
        strs.append(name(type) % '='_q % _N(get(type)));
    });
    return strs.join('|'_q);
}

auto VideoColor::fromString(const QString &str) -> VideoColor
{
    const auto strs = str.split('|'_q);
    QRegEx regex(uR"((\w+)=(\d+))"_q);
    VideoColor color;
    for (auto &one : strs) {
        auto match = regex.match(one);
        if (!match.hasMatch())
            return VideoColor();
        auto type = getType(match.captured(1));
        if (type == TypeMax)
            return VideoColor();
        color.set(type, match.captured(2).toInt());
    }
    return color;
}

auto VideoColor::toJson() const -> QJsonObject
{
    QJsonObject json;
    for_type([&] (Type type) { json.insert(name(type), get(type)); });
    return json;
}

auto VideoColor::setFromJson(const QJsonObject &json) -> bool
{
    bool ok = true;
    for_type([&] (Type type) {
        const auto it = json.find(name(type));
        if (it == json.end())
            ok = false;
        else
            set(type, (*it).toInt());
    });
    return ok;
}
