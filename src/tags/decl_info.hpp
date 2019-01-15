#pragma once

#include <string>
#include "context.hpp"

namespace is_decl_info {
    struct tag {};
    template <class T>
    struct trait : public std::integral_constant<bool, std::is_base_of<tag, T>::value> {};
}  // namespace is_decl_info

template <class Target, class _Context>
struct DeclInfo : is_decl_info::tag, _Context {
    static_assert(is_context::trait<_Context>::value, "_Context is not a context");
    using context = _Context;
    using target_type = Target;

    Target& target;

    DeclInfo(Target& target) : target(target) {}

    template <class Tag>
    DeclInfo<Target, typename add_tag<_Context, Tag>::type> yolo() {
        return DeclInfo<Target, typename add_tag<_Context, Tag>::type>(target);
    }
};

template <class... Tags, class Target>
DeclInfo<Target, Context<Tags...>> make_decl_info(Target& target) {
    return DeclInfo<Target, Context<Tags...>>(target);
}
