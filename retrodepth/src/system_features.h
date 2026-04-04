#pragma once

#include <string>
#include <string_view>

inline constexpr bool kEnableSmsSupport = true;
inline constexpr bool kEnableNesSupport = false;
inline constexpr bool kEnableSnesSupport = true;
inline constexpr bool kEnableGenesisSupport = true;
inline constexpr bool kEnableGbSupport = true;
inline constexpr bool kEnableGbcSupport = true;

inline bool is_system_enabled(std::string_view system) {
    if (system == "sms")     return kEnableSmsSupport;
    if (system == "nes")     return kEnableNesSupport;
    if (system == "snes")    return kEnableSnesSupport;
    if (system == "genesis") return kEnableGenesisSupport;
    if (system == "gb")      return kEnableGbSupport;
    if (system == "gbc")     return kEnableGbcSupport;
    return true;
}

inline bool uses_storage_key_prefix(std::string_view system) {
    return system == "sms"
        || system == "nes"
        || system == "snes"
        || system == "genesis"
        || system == "gb"
        || system == "gbc";
}

inline bool is_cart_system_id(std::string_view system) {
    return uses_storage_key_prefix(system) && is_system_enabled(system);
}

inline std::string make_storage_key_for_system(std::string_view system,
                                               const std::string& short_name) {
    if (uses_storage_key_prefix(system))
        return std::string(system) + "__" + short_name;
    return short_name;
}

inline bool rom_name_has_prefix(const std::string& rom_name,
                                std::string_view prefix) {
    return rom_name.size() >= prefix.size() &&
           rom_name.compare(0, prefix.size(), prefix) == 0;
}

inline bool is_sms_cart_rom_name(const std::string& rom_name) {
    return rom_name_has_prefix(rom_name, "sms -cart \"")
        || rom_name_has_prefix(rom_name, "sms1 -cart \"")
        || rom_name_has_prefix(rom_name, "smsj -cart \"")
        || rom_name_has_prefix(rom_name, "smskr -cart \"");
}

inline bool is_nes_cart_rom_name(const std::string& rom_name) {
    return rom_name_has_prefix(rom_name, "nes -cart \"");
}

inline bool is_snes_cart_rom_name(const std::string& rom_name) {
    return rom_name_has_prefix(rom_name, "snes -cart \"");
}

inline bool is_genesis_cart_rom_name(const std::string& rom_name) {
    return rom_name_has_prefix(rom_name, "genesis -cart \"");
}

inline bool is_gb_cart_rom_name(const std::string& rom_name) {
    return rom_name_has_prefix(rom_name, "gameboy -cart \"")
        || rom_name_has_prefix(rom_name, "gbpocket -cart \"");
}

inline bool is_gbc_cart_rom_name(const std::string& rom_name) {
    return rom_name_has_prefix(rom_name, "gbcolor -cart \"");
}

inline bool config_matches_cart_system(std::string_view system,
                                       const std::string& rom_name) {
    if (system == "sms")     return is_sms_cart_rom_name(rom_name);
    if (system == "nes")     return is_nes_cart_rom_name(rom_name);
    if (system == "snes")    return is_snes_cart_rom_name(rom_name);
    if (system == "genesis") return is_genesis_cart_rom_name(rom_name);
    if (system == "gb")      return is_gb_cart_rom_name(rom_name);
    if (system == "gbc")     return is_gbc_cart_rom_name(rom_name);
    return false;
}
