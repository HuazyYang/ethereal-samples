#ifndef SAMPLETYPES_H
#define SAMPLETYPES_H
#include <nvrhi/core/foundation.h>
#include <gvdb/GPDevice.h>
#include <donut/core/log.h>
#include <nvrhi/core/memory.h>

namespace SampleUtils {

#define UT_V_GP(expr)                                                      \
    do {                                                                     \
        auto rc = (expr);                                                    \
        if (NVRHI_FAILED(rc)) {                                                   \
            donut::log::error("GPDevice failed with error: %d", (int)rc); \
            NVRHI_ASSERT(0);                                              \
        }                                                                    \
    } while (0)

struct GPDeviceMessageCallback : public nvrhi::ObjectImpl<donut::gp::IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GPDeviceMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(donut::gp::IMessageCallback)
    NVRHI_END_INTERFACE_TABLE()

    void message(donut::gp::MessageSeverity severity, const char *desc) override {
        using namespace donut;
        log::Severity logSeverity;
        switch (severity) {
            default:
            case gp::MessageSeverity::Info:
                logSeverity = log::Severity::Info;
                break;
            case gp::MessageSeverity::Warning:
                logSeverity = log::Severity::Warning;
                break;
            case gp::MessageSeverity::Error:
                logSeverity = log::Severity::Error;
                break;
            case gp::MessageSeverity::Fatal:
                logSeverity = log::Severity::Fatal;
        }

        donut::log::message(logSeverity, "%s", desc);
    }
};

template <typename T>
class Range {
public:
    Range(T *start, T *end): _start{start}, _end{end}, _count{size_t(end - start)} {}
    Range(T *start, size_t n) : _start{start}, _end{start + n}, _count{n} {}

    T *data() { return _start; }
    const T *data() const { return _start; }

    size_t size() const { return _count; }

    T &operator[](size_t i) {
        if (i < _count) return _start[i];
        NVRHI_ASSERT(0 && "Invalid index");
        throw std::invalid_argument("Range invalid index");
    }

    const T &operator[](size_t i) const {
        if (i < _count) return _start[i];
        NVRHI_ASSERT(0 && "Invalid index");
        throw std::invalid_argument("Range invalid index");
    }

    T *begin() { return _start; }

    T *end() { return _end; }

    const T *begin() const { return _start; }
    const T *end() const { return _end; }

 private:
    T *_start, *_end;
    size_t _count;
};

};  // namespace SampleUtils

#endif /* SAMPLETYPES_H */
