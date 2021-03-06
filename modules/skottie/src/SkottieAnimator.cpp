/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCubicMap.h"
#include "SkottieJson.h"
#include "SkottiePriv.h"
#include "SkottieValue.h"
#include "SkSGScene.h"
#include "SkString.h"
#include "SkTArray.h"

#include <memory>

namespace skottie {
namespace internal {

namespace {

#define LOG SkDebugf

bool LogFail(const skjson::Value& json, const char* msg) {
    const auto dump = json.toString();
    LOG("!! %s: %s\n", msg, dump.c_str());
    return false;
}

class KeyframeAnimatorBase : public sksg::Animator {
public:
    int count() const { return fRecs.count(); }

protected:
    KeyframeAnimatorBase() = default;

    struct KeyframeRec {
        float t0, t1;
        int   vidx0, vidx1, // v0/v1 indices
              cmidx;        // cubic map index

        bool contains(float t) const { return t0 <= t && t <= t1; }
        bool isConstant() const { return vidx0 == vidx1; }
        bool isValid() const {
            SkASSERT(t0 <= t1);
            // Constant frames don't need/use t1 and vidx1.
            return t0 < t1 || this->isConstant();
        }
    };

    const KeyframeRec& frame(float t) {
        if (!fCachedRec || !fCachedRec->contains(t)) {
            fCachedRec = findFrame(t);
        }
        return *fCachedRec;
    }

    float localT(const KeyframeRec& rec, float t) const {
        SkASSERT(rec.isValid());
        SkASSERT(!rec.isConstant());
        SkASSERT(t > rec.t0 && t < rec.t1);

        auto lt = (t - rec.t0) / (rec.t1 - rec.t0);

        return rec.cmidx < 0
            ? lt
            : SkTPin(fCubicMaps[rec.cmidx].computeYFromX(lt), 0.0f, 1.0f);
    }

    virtual int parseValue(const skjson::Value&, const AnimationBuilder* abuilder) = 0;

    void parseKeyFrames(const skjson::ArrayValue& jframes, const AnimationBuilder* abuilder) {
        for (const skjson::ObjectValue* jframe : jframes) {
            if (!jframe) continue;

            float t0;
            if (!Parse<float>((*jframe)["t"], &t0))
                continue;

            if (!fRecs.empty()) {
                if (fRecs.back().t1 >= t0) {
                    LOG("!! Ignoring out-of-order key frame (t:%f < t:%f)\n", t0, fRecs.back().t1);
                    continue;
                }
                // Back-fill t1 in prev interval.  Note: we do this even if we end up discarding
                // the current interval (to support "t"-only final frames).
                fRecs.back().t1 = t0;
            }

            // Required start value.
            const auto v0_idx = this->parseValue((*jframe)["s"], abuilder);
            if (v0_idx < 0)
                continue;

            // Optional end value.
            const auto v1_idx = this->parseValue((*jframe)["e"], abuilder);
            if (v1_idx < 0) {
                // Constant keyframe.
                fRecs.push_back({t0, t0, v0_idx, v0_idx, -1 });
                continue;
            }

            // default is linear lerp
            static constexpr SkPoint kDefaultC0 = { 0, 0 },
                                     kDefaultC1 = { 1, 1 };
            const auto c0 = ParseDefault<SkPoint>((*jframe)["i"], kDefaultC0),
                       c1 = ParseDefault<SkPoint>((*jframe)["o"], kDefaultC1);

            int cm_idx = -1;
            if (c0 != kDefaultC0 || c1 != kDefaultC1) {
                // TODO: is it worth de-duping these?
                cm_idx = fCubicMaps.count();
                fCubicMaps.emplace_back();
                // TODO: why do we have to plug these inverted?
                fCubicMaps.back().setPts(c1, c0);
            }

            fRecs.push_back({t0, t0, v0_idx, v1_idx, cm_idx });
        }

        // If we couldn't determine a valid t1 for the last frame, discard it.
        if (!fRecs.empty() && !fRecs.back().isValid()) {
            fRecs.pop_back();
        }

        SkASSERT(fRecs.empty() || fRecs.back().isValid());
    }

private:
    const KeyframeRec* findFrame(float t) const {
        SkASSERT(!fRecs.empty());

        auto f0 = &fRecs.front(),
             f1 = &fRecs.back();

        SkASSERT(f0->isValid());
        SkASSERT(f1->isValid());

        if (t < f0->t0) {
            return f0;
        }

        if (t > f1->t1) {
            return f1;
        }

        while (f0 != f1) {
            SkASSERT(f0 < f1);
            SkASSERT(t >= f0->t0 && t <= f1->t1);

            const auto f = f0 + (f1 - f0) / 2;
            SkASSERT(f->isValid());

            if (t > f->t1) {
                f0 = f + 1;
            } else {
                f1 = f;
            }
        }

        SkASSERT(f0 == f1);
        SkASSERT(f0->contains(t));

        return f0;
    }

    SkTArray<KeyframeRec> fRecs;
    SkTArray<SkCubicMap>  fCubicMaps;
    const KeyframeRec*    fCachedRec = nullptr;

    using INHERITED = sksg::Animator;
};

template <typename T>
class KeyframeAnimator final : public KeyframeAnimatorBase {
public:
    static std::unique_ptr<KeyframeAnimator> Make(const skjson::ArrayValue* jv,
                                                  const AnimationBuilder* abuilder,
                                                  std::function<void(const T&)>&& apply) {
        if (!jv) return nullptr;

        std::unique_ptr<KeyframeAnimator> animator(
            new KeyframeAnimator(*jv, abuilder, std::move(apply)));
        if (!animator->count())
            return nullptr;

        return animator;
    }

protected:
    void onTick(float t) override {
        fApplyFunc(*this->eval(this->frame(t), t, &fScratch));
    }

private:
    KeyframeAnimator(const skjson::ArrayValue& jframes,
                     const AnimationBuilder* abuilder,
                     std::function<void(const T&)>&& apply)
        : fApplyFunc(std::move(apply)) {
        this->parseKeyFrames(jframes, abuilder);
    }

    int parseValue(const skjson::Value& jv, const AnimationBuilder* abuilder) override {
        T val;
        if (!ValueTraits<T>::FromJSON(jv, abuilder, &val) ||
            (!fVs.empty() && !ValueTraits<T>::CanLerp(val, fVs.back()))) {
            return -1;
        }

        // TODO: full deduping?
        if (fVs.empty() || val != fVs.back()) {
            fVs.push_back(std::move(val));
        }
        return fVs.count() - 1;
    }

    const T* eval(const KeyframeRec& rec, float t, T* v) const {
        SkASSERT(rec.isValid());
        if (rec.isConstant() || t <= rec.t0) {
            return &fVs[rec.vidx0];
        } else if (t >= rec.t1) {
            return &fVs[rec.vidx1];
        }

        const auto lt = this->localT(rec, t);
        const auto& v0 = fVs[rec.vidx0];
        const auto& v1 = fVs[rec.vidx1];
        ValueTraits<T>::Lerp(v0, v1, lt, v);

        return v;
    }

    const std::function<void(const T&)> fApplyFunc;
    SkTArray<T>                         fVs;

    // LERP storage: we use this to temporarily store interpolation results.
    // Alternatively, the temp result could live on the stack -- but for vector values that would
    // involve dynamic allocations on each tick.  This a trade-off to avoid allocator pressure
    // during animation.
    T                                   fScratch; // lerp storage

    using INHERITED = KeyframeAnimatorBase;
};

template <typename T>
static inline bool BindPropertyImpl(const skjson::ObjectValue* jprop,
                                    const AnimationBuilder* abuilder,
                                    AnimatorScope* ascope,
                                    std::function<void(const T&)>&& apply,
                                    const T* noop = nullptr) {
    if (!jprop) return false;

    const auto& jpropA = (*jprop)["a"];
    const auto& jpropK = (*jprop)["k"];

    if (!(*jprop)["x"].is<skjson::NullValue>()) {
        LOG("?? Unsupported expression.\n");
    }

    // Older Json versions don't have an "a" animation marker.
    // For those, we attempt to parse both ways.
    if (!ParseDefault<bool>(jpropA, false)) {
        T val;
        if (ValueTraits<T>::FromJSON(jpropK, abuilder, &val)) {
            // Static property.
            if (noop && val == *noop)
                return false;

            apply(val);
            return true;
        }

        if (!jpropA.is<skjson::NullValue>()) {
            return LogFail(*jprop, "Could not parse (explicit) static property");
        }
    }

    // Keyframe property.
    auto animator = KeyframeAnimator<T>::Make(jpropK, abuilder, std::move(apply));

    if (!animator) {
        return LogFail(*jprop, "Could not parse keyframed property");
    }

    ascope->push_back(std::move(animator));

    return true;
}

class SplitPointAnimator final : public sksg::Animator {
public:
    static std::unique_ptr<SplitPointAnimator> Make(const skjson::ObjectValue* jprop,
                                                    const AnimationBuilder* abuilder,
                                                    std::function<void(const VectorValue&)>&& apply,
                                                    const VectorValue*) {
        if (!jprop) return nullptr;

        std::unique_ptr<SplitPointAnimator> split_animator(
            new SplitPointAnimator(std::move(apply)));

        // This raw pointer is captured in lambdas below. But the lambdas are owned by
        // the object itself, so the scope is bound to the life time of the object.
        auto* split_animator_ptr = split_animator.get();

        if (!BindPropertyImpl<ScalarValue>((*jprop)["x"], abuilder, &split_animator->fAnimators,
                [split_animator_ptr](const ScalarValue& x) { split_animator_ptr->setX(x); }) ||
            !BindPropertyImpl<ScalarValue>((*jprop)["y"], abuilder, &split_animator->fAnimators,
                [split_animator_ptr](const ScalarValue& y) { split_animator_ptr->setY(y); })) {
            LogFail(*jprop, "Could not parse split property");
            return nullptr;
        }

        if (split_animator->fAnimators.empty()) {
            // Static split property: commit the (buffered) value and discard.
            split_animator->onTick(0);
            return nullptr;
        }

        return split_animator;
    }

    void onTick(float t) override {
        for (const auto& animator : fAnimators) {
            animator->tick(t);
        }

        const VectorValue vec = { fX, fY };
        fApplyFunc(vec);
    }

    void setX(const ScalarValue& x) { fX = x; }
    void setY(const ScalarValue& y) { fY = y; }

private:
    explicit SplitPointAnimator(std::function<void(const VectorValue&)>&& apply)
        : fApplyFunc(std::move(apply)) {}

    const std::function<void(const VectorValue&)> fApplyFunc;
    sksg::AnimatorList                            fAnimators;

    ScalarValue                                   fX = 0,
                                                  fY = 0;

    using INHERITED = sksg::Animator;
};

bool BindSplitPositionProperty(const skjson::Value& jv,
                               const AnimationBuilder* abuilder,
                               AnimatorScope* ascope,
                               std::function<void(const VectorValue&)>&& apply,
                               const VectorValue* noop) {
    if (auto split_animator = SplitPointAnimator::Make(jv, abuilder, std::move(apply), noop)) {
        ascope->push_back(std::unique_ptr<sksg::Animator>(split_animator.release()));
        return true;
    }

    return false;
}

} // namespace

template <>
bool AnimationBuilder::bindProperty(const skjson::Value& jv,
                  AnimatorScope* ascope,
                  std::function<void(const ScalarValue&)>&& apply,
                  const ScalarValue* noop) const {
    return BindPropertyImpl(jv, this, ascope, std::move(apply), noop);
}

template <>
bool AnimationBuilder::bindProperty(const skjson::Value& jv,
                  AnimatorScope* ascope,
                  std::function<void(const VectorValue&)>&& apply,
                  const VectorValue* noop) const {
    if (!jv.is<skjson::ObjectValue>())
        return false;

    return ParseDefault<bool>(jv.as<skjson::ObjectValue>()["s"], false)
        ? BindSplitPositionProperty(jv, this, ascope, std::move(apply), noop)
        : BindPropertyImpl(jv, this, ascope, std::move(apply), noop);
}

template <>
bool AnimationBuilder::bindProperty(const skjson::Value& jv,
                  AnimatorScope* ascope,
                  std::function<void(const ShapeValue&)>&& apply,
                  const ShapeValue* noop) const {
    return BindPropertyImpl(jv, this, ascope, std::move(apply), noop);
}

template <>
bool AnimationBuilder::bindProperty(const skjson::Value& jv,
                  AnimatorScope* ascope,
                  std::function<void(const TextValue&)>&& apply,
                  const TextValue* noop) const {
    return BindPropertyImpl(jv, this, ascope, std::move(apply), noop);
}

} // namespace internal
} // namespace skottie
