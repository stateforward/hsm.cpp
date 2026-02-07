#ifndef HSM_DETAIL_PATH_HPP
#define HSM_DETAIL_PATH_HPP

#include <array>
#include <string_view>
#include <type_traits>

#include "fixed_string.hpp"

namespace hsm::detail::path
{

namespace detail
{
template <typename CharT>
[[nodiscard]] constexpr bool
is_separator (CharT ch) noexcept
{
  return ch == '/';
}

template <auto Path>
requires fixed_string_constant<Path> consteval bool
has_relative_segments ()
{
  std::size_t index = 0;
  const auto length = Path.size ();
  while (index < length)
    {
      while (index < length && is_separator (Path[index]))
        {
          ++index;
        }
      const auto start = index;
      while (index < length && !is_separator (Path[index]))
        {
          ++index;
        }
      const auto segment_len = index - start;
      if (segment_len == 1 && Path[start] == '.')
        {
          return true;
        }
      if (segment_len == 2 && Path[start] == '.' && Path[start + 1] == '.')
        {
          return true;
        }
    }
  return false;
}

template <auto Path>
requires fixed_string_constant<Path> consteval bool
is_absolute_literal ()
{
  return Path.size () > 0 && Path[0] == '/';
}

template <auto Path>
requires fixed_string_constant<Path> consteval auto
normalize_literal ()
{
  static_assert (Path.size () > 0, "hsm paths cannot be empty");
  static_assert (is_absolute_literal<Path> (),
                 "hsm absolute paths must start with '/'");
  static_assert (!has_relative_segments<Path> (),
                 "Relative path segments ('.' or '..') are not supported in "
                 "hsm absolute paths");

  struct buffer_t
  {
    std::array<char, Path.size () + 1> data{};
    std::size_t length{};
  };

  constexpr auto normalized = [] {
    buffer_t buffer{};
    buffer.data[buffer.length++] = '/';

    const std::size_t total_length = Path.size ();
    std::size_t read = 1;

    while (read < total_length)
      {
        while (read < total_length && is_separator (Path[read]))
          {
            ++read;
          }
        if (read >= total_length)
          {
            break;
          }

        std::array<char, Path.size () + 1> segment{};
        std::size_t segment_len = 0;
        while (read < total_length && !is_separator (Path[read]))
          {
            segment[segment_len++] = Path[read++];
          }

        if (segment_len == 0)
          {
            continue;
          }

        if (buffer.length > 0
            && !is_separator (buffer.data[buffer.length - 1]))
          {
            buffer.data[buffer.length++] = '/';
          }

        for (std::size_t i = 0; i < segment_len; ++i)
          {
            buffer.data[buffer.length++] = segment[i];
          }
      }

    if (buffer.length > 1 && is_separator (buffer.data[buffer.length - 1]))
      {
        --buffer.length;
      }

    buffer.data[buffer.length] = '\0';
    return buffer;
  }();

  return fs_detail::make_fixed_string_from_array (
      normalized.data,
      std::integral_constant<std::size_t, normalized.length>{});
}
} // namespace detail

template <fixed_string_literal Name> struct root_path
{
  static constexpr auto value = [] () consteval {
    static_assert (Name::size () > 0, "model names cannot be empty");
    std::array<char, Name::size () + 2> buffer{};
    buffer[0] = '/';
    for (std::size_t i = 0; i < Name::size (); ++i)
      {
        buffer[i + 1] = Name::value[i];
      }
    buffer[Name::size () + 1] = '\0';
    return fs_detail::make_fixed_string_from_array (
        buffer, std::integral_constant<std::size_t, Name::size () + 1>{});
  }();

  using type = decltype (value);
};

template <fixed_string_literal Name>
inline constexpr auto root_path_v = root_path<Name>::value;

template <fixed_string_literal Parent, fixed_string_literal Child>
struct append_child_path
{
  static constexpr auto value = [] () consteval {
    constexpr std::size_t parent_len = Parent::size ();
    constexpr std::size_t child_len = Child::size ();
    struct buffer_t
    {
      std::array<char, parent_len + child_len + 2> data{};
      std::size_t length{};
    };

    constexpr auto appended = [] () consteval {
      buffer_t buffer{};
      for (; buffer.length < parent_len; ++buffer.length)
        {
          buffer.data[buffer.length] = Parent::value[buffer.length];
        }
      if (parent_len > 1)
        {
          buffer.data[buffer.length++] = '/';
        }
      for (std::size_t i = 0; i < child_len; ++i)
        {
          buffer.data[buffer.length++] = Child::value[i];
        }
      buffer.data[buffer.length] = '\0';
      return buffer;
    }();

    return fs_detail::make_fixed_string_from_array (
        appended.data, std::integral_constant<std::size_t, appended.length>{});
  }();

  using type = decltype (value);
};

template <fixed_string_literal Parent, fixed_string_literal Child>
inline constexpr auto append_child_path_v
    = append_child_path<Parent, Child>::value;

[[nodiscard]] constexpr bool
is_absolute (std::string_view path) noexcept
{
  return !path.empty () && path.front () == '/';
}

template <fixed_string_literal Path>
[[nodiscard]] constexpr bool
is_absolute (Path) noexcept
{
  return Path::size () > 0 && Path::value[0] == '/';
}

[[nodiscard]] constexpr bool
is_root (std::string_view path) noexcept
{
  return path == "/";
}

[[nodiscard]] constexpr bool
is_ancestor_or_equal (std::string_view ancestor,
                      std::string_view descendant) noexcept
{
  if (!is_absolute (ancestor) || !is_absolute (descendant))
    {
      return false;
    }
  if (ancestor == "/")
    {
      return true;
    }
  if (ancestor.size () > descendant.size ())
    {
      return false;
    }
  if (!descendant.starts_with (ancestor))
    {
      return false;
    }
  if (ancestor.size () == descendant.size ())
    {
      return true;
    }
  return descendant[ancestor.size ()] == '/';
}

[[nodiscard]] constexpr bool
is_child (std::string_view parent, std::string_view child) noexcept
{
  if (!is_absolute (parent) || !is_absolute (child) || parent == child)
    {
      return false;
    }
  if (parent == "/")
    {
      auto slash_pos = child.find ('/', 1);
      return slash_pos == std::string_view::npos;
    }
  if (!child.starts_with (parent))
    {
      return false;
    }
  const auto separator_index = parent.size ();
  if (separator_index >= child.size () || child[separator_index] != '/')
    {
      return false;
    }
  return child.find ('/', separator_index + 1) == std::string_view::npos;
}

template <auto Path>
requires fixed_string_constant<Path> struct normalized_path
{
  static constexpr auto value = detail::normalize_literal<Path> ();

  [[nodiscard]] static constexpr std::string_view
  view () noexcept
  {
    return value.view ();
  }
};

template <auto Path>
requires fixed_string_constant<Path> inline constexpr auto normalized_path_v
    = normalized_path<Path>::value;

template <fixed_string_literal Name>
[[nodiscard]] consteval auto
normalize (Name)
{
  return detail::normalize_literal<Name{}> ();
}

} // namespace hsm::detail::path

#endif // HSM_DETAIL_PATH_HPP
