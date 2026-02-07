#ifndef HSM_DETAIL_ASSERT_HPP
#define HSM_DETAIL_ASSERT_HPP

#include <cstddef>
#include <exception>

#include "fixed_string.hpp"

namespace hsm::detail
{

inline void SEE_CONSTEXPR_ASSERT_MESSAGE() {}

// constexpr_assert is used inside constexpr/consteval normalization code as a
// last-resort check for structurally invalid models (for example, unresolved
// target paths). It must not rely on exceptions, even when they are enabled;
// instead we terminate at runtime and intentionally trigger a compile-time
// failure when evaluated in a constant-expression context.
template <std::size_t N>
[[noreturn]] constexpr void constexpr_assert(const char (&)[N]) noexcept
{
  SEE_CONSTEXPR_ASSERT_MESSAGE();
  std::terminate();
}

// Name/path helpers keep call sites readable. The specific diagnostic text is
// carried at the call site; constexpr_assert itself is message-agnostic.
template <std::size_t N1, std::size_t N2>
constexpr void assert_name_contains_no_slash(const char (&name)[N1],
                                             const char (&)[N2])
{
  if constexpr (N1 > 1)
    {
      for (std::size_t i = 0; i < N1 - 1; ++i)
        {
          if (name[i] == '/')
            {
              constexpr_assert("hsm name must not contain '/'");
            }
        }
    }
}

template <std::size_t N1, std::size_t N2>
constexpr void assert_path_absolute(const char (&path)[N1],
                                    const char (&)[N2])
{
  if constexpr (N1 > 1)
    {
      if (path[0] != '/')
        {
          constexpr_assert("hsm target path must be absolute");
        }
    }
}

} // namespace hsm::detail

#endif // HSM_DETAIL_ASSERT_HPP
