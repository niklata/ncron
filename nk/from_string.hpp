#ifndef NKLIB_FROM_STRING_HPP_
#define NKLIB_FROM_STRING_HPP_

#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <limits>
#include <type_traits>
#include <string>
#include <string_view>
#include <charconv>

namespace nk {
    namespace detail {
        template <typename T>
        [[nodiscard]] constexpr bool str_to_signed_integer(const char *s, T *result)
        {
            using ut = typename std::make_unsigned<T>::type;
            constexpr auto maxut = static_cast<typename std::make_unsigned<T>::type>(std::numeric_limits<T>::max());
            ut ret(0), digit(0);
            const bool neg = (*s == '-');
            if (neg) ++s;
            if (!(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
                do {
                    if (*s < '0' || *s > '9')
                        return false;
                    if (ret > maxut / 10) {
                        ret = std::numeric_limits<ut>::max();
                        break;
                    }
                    digit = static_cast<ut>(*s) - '0';
                    ret = ret * 10u + digit;
                } while (*++s);
            } else {
                s += 2;
                do {
                    if (*s >= '0' && *s <= '9')
                        digit = static_cast<ut>(*s) - '0';
                    else if (*s >= 'A' && *s <= 'F')
                        digit = static_cast<ut>(*s) - 'A' + ut{ 10 };
                    else if (*s >= 'a' && *s <= 'f')
                        digit = static_cast<ut>(*s) - 'a' + ut{ 10 };
                    else
                        return false;
                    if (ret > maxut / 16) {
                        ret = std::numeric_limits<ut>::max();
                        break;
                    }
                    ret = ret * 16u + digit;
                } while (*++s);
            }
            if (ret > maxut + neg)
                return false; // out of range
            *result = neg ? -static_cast<T>(ret) : static_cast<T>(ret);
            return true;
        }
        template <typename T>
        [[nodiscard]] constexpr bool str_to_signed_integer(const char *s, size_t c, T *result)
        {
            using ut = typename std::make_unsigned<T>::type;
            constexpr auto maxut = static_cast<typename std::make_unsigned<T>::type>(std::numeric_limits<T>::max());
            ut ret(0), digit(0);
            if (c == 0)
                return false;
            const ut neg = (*s == '-');
            if (neg) ++s, c--;
            if (c == 0)
                return false;
            if (!(c > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
                do {
                    if (*s < '0' || *s > '9')
                        return false;
                    if (ret > maxut / 10) {
                        ret = std::numeric_limits<ut>::max();
                        break;
                    }
                    digit = static_cast<ut>(*s) - '0';
                    ret = ret * 10u + digit;
                } while (++s, --c);
            } else {
                s += 2; c -= 2;
                do {
                    if (*s >= '0' && *s <= '9')
                        digit = static_cast<ut>(*s) - '0';
                    else if (*s >= 'A' && *s <= 'F')
                        digit = static_cast<ut>(*s) - 'A' + ut{ 10 };
                    else if (*s >= 'a' && *s <= 'f')
                        digit = static_cast<ut>(*s) - 'a' + ut{ 10 };
                    else
                        return false;
                    if (ret > maxut / 16) {
                        ret = std::numeric_limits<ut>::max();
                        break;
                    }
                    ret = ret * 16u + digit;
                } while (++s, --c);
            }
            if (ret > maxut + neg)
                return false; // out of range
            *result = neg ? -static_cast<T>(ret) : static_cast<T>(ret);
            return true;
        }
        template <typename T>
        [[nodiscard]] constexpr bool str_to_unsigned_integer(const char *s, T *result)
        {
            T ret(0), digit(0);
            const bool neg = (*s == '-');
            if (neg)
                return false; // out of range
            if (!(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
                do {
                    if (*s < '0' || *s > '9')
                        return false;
                    if (ret > std::numeric_limits<T>::max() / 10)
                        return false; // out of range
                    digit = static_cast<T>(*s) - '0';
                    ret = ret * 10u + digit;
                } while (*++s);
            } else {
                s += 2;
                do {
                    if (*s >= '0' && *s <= '9')
                        digit = static_cast<T>(*s) - '0';
                    else if (*s >= 'A' && *s <= 'F')
                        digit = static_cast<T>(*s) - 'A' + T{ 10 };
                    else if (*s >= 'a' && *s <= 'f')
                        digit = static_cast<T>(*s) - 'a' + T{ 10 };
                    else
                        return false;
                    if (ret > std::numeric_limits<T>::max() / 16)
                        return false; // out of range
                    ret = ret * 16u + digit;
                } while (*++s);
            }
            *result = ret;
            return true;
        }
        template <typename T>
        [[nodiscard]] constexpr bool str_to_unsigned_integer(const char *s, size_t c, T *result)
        {
            T ret(0), digit(0);
            if (c == 0)
                return false;
            const bool neg = (*s == '-');
            if (neg)
                return false; // out of range
            if (!(c > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
                do {
                    if (*s < '0' || *s > '9')
                        return false;
                    if (ret > std::numeric_limits<T>::max() / 10)
                        return false; // out of range
                    digit = static_cast<T>(*s) - '0';
                    ret = ret * 10u + digit;
                } while (++s, --c);
            } else {
                s += 2; c -= 2;
                do {
                    if (*s >= '0' && *s <= '9')
                        digit = static_cast<T>(*s) - '0';
                    else if (*s >= 'A' && *s <= 'F')
                        digit = static_cast<T>(*s) - 'A' + T{ 10 };
                    else if (*s >= 'a' && *s <= 'f')
                        digit = static_cast<T>(*s) - 'a' + T{ 10 };
                    else
                        return false;
                    if (ret > std::numeric_limits<T>::max() / 16)
                        return false; // out of range
                    ret = ret * 16u + digit;
                } while (++s, --c);
            }
            *result = ret;
            return true;
        }

        [[nodiscard]] static inline bool str_to_double(const char *s, size_t c, double *result)
        {
            double v;
            const auto ret = std::from_chars(s, s + c, v);
            if (ret.ec == std::errc{}) {
                *result = v;
                return true;
            }
            return false;
        }
        [[nodiscard]] static inline bool str_to_float(const char *s, size_t c, float *result)
        {
            float v;
            const auto ret = std::from_chars(s, s + c, v);
            if (ret.ec == std::errc{}) {
                *result = v;
                return true;
            }
            return false;
        }
        [[nodiscard]] static inline bool str_to_long_double(const char *s, size_t c, long double *result)
        {
            long double v;
            const auto ret = std::from_chars(s, s + c, v);
            if (ret.ec == std::errc{}) {
                *result = v;
                return true;
            }
            return false;
        }
        [[nodiscard]] static inline bool str_to_double(const char *s, double *result)
        {
            return str_to_double(s, strlen(s), result);
        }
        [[nodiscard]] static inline bool str_to_float(const char *s, float *result)
        {
            return str_to_float(s, strlen(s), result);
        }
        [[nodiscard]] static inline bool str_to_long_double(const char *s, long double *result)
        {
            return str_to_long_double(s, strlen(s), result);
        }

        template <typename T>
        [[nodiscard]] bool do_from_string(const char *s, T *result)
        {
            static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>, "T must be integer or floating point type");
            if constexpr (std::is_integral_v<T>) {
                if constexpr (std::is_signed_v<T>) {
                    return detail::str_to_signed_integer<T>(s, result);
                } else {
                    return detail::str_to_unsigned_integer<T>(s, result);
                }
            } else if constexpr (std::is_floating_point_v<T>) {
                if constexpr (std::is_same_v<typename std::remove_cv<T>::type, double>) {
                    return str_to_double(s, result);
                } else if constexpr (std::is_same_v<typename std::remove_cv<T>::type, float>) {
                    return str_to_float(s, result);
                } else if constexpr (std::is_same_v<typename std::remove_cv<T>::type, long double>) {
                    return str_to_long_double(s, result);
                }
            }
        }
        template <typename T>
        [[nodiscard]] bool do_from_string(const char *s, size_t c, T *result)
        {
            static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>, "T must be integer or floating point type");
            if constexpr (std::is_integral_v<T>) {
                if constexpr (std::is_signed_v<T>) {
                    return detail::str_to_signed_integer<T>(s, c, result);
                } else {
                    return detail::str_to_unsigned_integer<T>(s, c, result);
                }
            } else if constexpr (std::is_floating_point_v<T>) {
                if constexpr (std::is_same_v<typename std::remove_cv<T>::type, double>) {
                    return str_to_double(s, c, result);
                } else if constexpr (std::is_same_v<typename std::remove_cv<T>::type, float>) {
                    return str_to_float(s, c, result);
                } else if constexpr (std::is_same_v<typename std::remove_cv<T>::type, long double>) {
                    return str_to_long_double(s, c, result);
                }
            }
        }
    }

    template <typename T>
    [[nodiscard]] bool from_string(const char *s, T *result)
    {
        return detail::do_from_string<T>(s, result);
    }
    template <typename T>
    [[nodiscard]] bool from_string(const char *s, size_t c, T *result)
    {
        return detail::do_from_string<T>(s, c, result);
    }
    template <typename T>
    [[nodiscard]] bool from_string(const std::string &s, T *result)
    {
        return detail::do_from_string<T>(s.data(), s.size(), result);
    }
    template <typename T>
    [[nodiscard]] bool from_string(std::string_view s, T *result)
    {
        return detail::do_from_string<T>(s.data(), s.size(), result);
    }
}
#endif
