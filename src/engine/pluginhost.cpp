#include "pluginhost.h"
#include "../core/effectdefs.h"
#include "../plugin_api/oe_plugin.h"
#include <QDir>
#include <QHash>
#include <QLibrary>

namespace {
using ApiFn = int (*)();
using CountFn = int (*)();
using EffFn = const OeEffectDef *(*)(int);
using VidFn = void (*)(const char *, unsigned char *, int, int, int, const double *, double);
using AudFn = void (*)(const char *, float *, int, const double *, void **);
using FreeFn = void (*)(const char *, void *);
struct Entry { QByteArray id; VidFn vid = nullptr; AudFn aud = nullptr; FreeFn fre = nullptr; QStringList params; };
QHash<QString, Entry> table; // written only at startup, read-only once threads run
QStringList infos, errs;

QVector<double> evalParams(const Entry &en, const Effect &e, Frame kt)
{
    QVector<double> v;
    for (auto &id : en.params) v << e.val(id, kt);
    return v;
}
}

namespace PluginHost {
int loadFrom(const QString &dirPath)
{
    QDir dir(dirPath);
    int added = 0;
    for (auto &f : dir.entryInfoList({"*.dll", "*.so", "*.dylib"}, QDir::Files)) {
        auto *lib = new QLibrary(f.absoluteFilePath()); // stays loaded for the app's lifetime
        if (!lib->load()) { errs << f.fileName() + ": " + lib->errorString(); delete lib; continue; }
        auto api = reinterpret_cast<ApiFn>(lib->resolve("oe_plugin_api_version"));
        auto cnt = reinterpret_cast<CountFn>(lib->resolve("oe_plugin_effect_count"));
        auto eff = reinterpret_cast<EffFn>(lib->resolve("oe_plugin_effect"));
        if (!api || !cnt || !eff) { errs << f.fileName() + ": not an Open Editor plugin"; lib->unload(); delete lib; continue; }
        if (api() != OE_PLUGIN_API_VERSION) { errs << f.fileName() + QString(": API version %1 not supported").arg(api()); lib->unload(); delete lib; continue; }
        auto vid = reinterpret_cast<VidFn>(lib->resolve("oe_plugin_process_video"));
        auto aud = reinterpret_cast<AudFn>(lib->resolve("oe_plugin_process_audio"));
        auto fre = reinterpret_cast<FreeFn>(lib->resolve("oe_plugin_free_state"));
        for (int i = 0; i < cnt(); ++i) {
            const OeEffectDef *d = eff(i);
            if (!d || !d->id || (d->audio ? !aud : !vid)) continue;
            QString id = QString("plugin.") + d->id;
            if (table.contains(id)) { errs << f.fileName() + ": duplicate effect " + id; continue; }
            EffectDef def;
            def.id = id; def.name = d->name; def.category = d->category ? d->category : "Plugins"; def.audio = d->audio != 0;
            Entry en;
            en.id = d->id; en.vid = vid; en.aud = aud; en.fre = fre;
            for (int k = 0; k < d->paramCount; ++k) {
                const OeParamDef &p = d->params[k];
                ParamDef pd;
                pd.id = p.id; pd.name = p.name; pd.def = p.def; pd.min = p.min; pd.max = p.max; pd.softMin = p.min; pd.softMax = p.max;
                pd.step = (p.max - p.min) / 200;
                def.params << pd;
                en.params << pd.id;
            }
            EffectRegistry::add(def);
            table.insert(id, en);
            infos << f.fileName() + ": " + def.name;
            ++added;
        }
    }
    return added;
}
QStringList loaded() { return infos; }
QStringList errors() { return errs; }

bool video(const Effect &e, QImage &img, Frame kt, double seconds)
{
    auto it = table.constFind(e.type);
    if (it == table.constEnd() || !it->vid) return false;
    auto p = evalParams(*it, e, kt);
    it->vid(it->id.constData(), img.bits(), img.width(), img.height(), img.bytesPerLine(), p.constData(), seconds);
    return true;
}
bool audio(const Effect &e, float *s, int frames, Frame kt, std::shared_ptr<void> &state)
{
    auto it = table.constFind(e.type);
    if (it == table.constEnd() || !it->aud) return false;
    auto p = evalParams(*it, e, kt);
    void *raw = state.get();
    it->aud(it->id.constData(), s, frames, p.constData(), &raw);
    if (raw != state.get()) {
        Entry en = *it;
        state = std::shared_ptr<void>(raw, [en](void *x) { if (en.fre) en.fre(en.id.constData(), x); });
    }
    return true;
}
}
