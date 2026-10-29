// ES40 emulator.
#ifndef ES40_X11_HOTKEY_H
#define ES40_X11_HOTKEY_H

#include <cctype>
#include <cstdint>
#include <string>

enum
{
  X11_HOTKEY_CTRL = 1,
  X11_HOTKEY_ALT = 2,
  X11_HOTKEY_SHIFT = 4,
  X11_HOTKEY_GUI = 8,
  X11_HOTKEY_UNSUPPORTED = 16
};

struct x11_hotkey_key
{
  std::uint32_t key = 0;
  std::string name;
};

struct x11_hotkey_binding
{
  std::uint32_t key = 0;
  unsigned modifiers = 0;
  bool enabled = false, invalid = false;
  std::string display;

  bool matches(std::uint32_t host_key, unsigned host_modifiers) const
  {
    return enabled && key == host_key && modifiers == host_modifiers;
  }
};

inline bool x11_hotkey_equal(const std::string& text, const char* name)
{
  if(!name || text.size() != std::char_traits<char>::length(name)) return false;
  for(std::size_t i = 0; i < text.size(); ++i)
    if(std::tolower(static_cast<unsigned char>(text[i])) !=
       std::tolower(static_cast<unsigned char>(name[i]))) return false;
  return true;
}

namespace x11_hotkey_detail
{
inline std::string trim(const std::string& text)
{
  std::size_t first = 0, last = text.size();
  while(first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  while(last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
  return text.substr(first, last - first);
}

inline unsigned modifier(const std::string& text)
{
  if(x11_hotkey_equal(text, "Ctrl") || x11_hotkey_equal(text, "Control")) return X11_HOTKEY_CTRL;
  if(x11_hotkey_equal(text, "Alt") || x11_hotkey_equal(text, "Option")) return X11_HOTKEY_ALT;
  if(x11_hotkey_equal(text, "Shift")) return X11_HOTKEY_SHIFT;
  const char* names[] = { "GUI", "Super", "Win", "Windows", "Cmd", "Command", "Meta" };
  for(const char* name : names)
    if(x11_hotkey_equal(text, name)) return X11_HOTKEY_GUI;
  return 0;
}
}

template<class Resolve>
x11_hotkey_binding parse_x11_hotkey(const char* configured, Resolve resolver)
{
  x11_hotkey_binding binding;
  binding.display = "none";
  std::string key_name = x11_hotkey_detail::trim(configured ? configured : "");
  if(x11_hotkey_equal(key_name, "none")) return binding;
  binding.invalid = true;
  unsigned modifiers = 0;
  for(;;)
  {
    const std::size_t separator = key_name.find('+');
    if(separator == std::string::npos) break;
    const unsigned modifier = x11_hotkey_detail::modifier(
        x11_hotkey_detail::trim(key_name.substr(0, separator)));
    if(!modifier) break;
    if(modifiers & modifier) return binding;
    modifiers |= modifier;
    key_name = x11_hotkey_detail::trim(key_name.substr(separator + 1));
  }
  if(key_name.empty() || x11_hotkey_detail::modifier(key_name)) return binding;
  const auto resolved = resolver(key_name);
  if(!resolved.key) return binding;
  binding.key = resolved.key;
  binding.modifiers = modifiers;
  binding.enabled = true;
  binding.invalid = false;
  binding.display.clear();
  if(modifiers & X11_HOTKEY_CTRL) binding.display += "Ctrl+";
  if(modifiers & X11_HOTKEY_ALT) binding.display += "Alt+";
  if(modifiers & X11_HOTKEY_SHIFT) binding.display += "Shift+";
  if(modifiers & X11_HOTKEY_GUI) binding.display += "GUI+";
  binding.display += resolved.name;
  return binding;
}

#endif
