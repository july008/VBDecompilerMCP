/*
 * vbdmcp_mem.hpp - C++ RAII 内存助手（MCP 服务器模块统一使用）
 * vbdmcp_mem.hpp - C++ RAII memory helpers (used across the MCP server modules)
 *
 * 凡是"申请 → 用 → 释放"的资源都用 RAII 管理，避免异常路径 / 提前 return 时泄漏。
 * 底层仍是 CRT malloc/free（与 VB Decompiler 进程模型一致），这里只提供类型安全的
 * 智能指针封装，不改变分配器语义。
 *
 * Every "acquire -> use -> release" resource is managed with RAII so exception
 * paths and early returns cannot leak it. The underlying allocator is still CRT
 * malloc/free (matching the VB Decompiler process model); only type-safe smart
 * pointer wrappers are added here, allocator semantics are unchanged.
 */
#ifndef VBDMCP_MEM_HPP
#define VBDMCP_MEM_HPP

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace vbdmcp {

/* free 删除器：配合 std::unique_ptr 管理 vbdmcp_alloc / malloc 的裸指针。
 * free deleter: lets std::unique_ptr own raw pointers from vbdmcp_alloc / malloc. */
struct FreeDeleter {
    void operator()(void* p) const noexcept { if (p) ::free(p); }
};

/* char* 的所有权封装，按 vbdmcp_free 语义释放。
 * Owning char* wrapper, released with vbdmcp_free semantics. */
using OwnedString = std::unique_ptr<char, FreeDeleter>;

/* RAII 套接字：析构时自动 closesocket，异常路径不会漏关。
 * RAII socket: closes itself on destruction, so exception paths cannot leak it. */
class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET s) : sock_(s) {}
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& o) noexcept : sock_(o.release()) {}
    SocketHandle& operator=(SocketHandle&& o) noexcept
    {
        if (this != &o) reset(o.release());
        return *this;
    }
    ~SocketHandle() { reset(); }

    SOCKET get() const noexcept { return sock_; }
    SOCKET release() noexcept { SOCKET s = sock_; sock_ = INVALID_SOCKET; return s; }
    void reset(SOCKET s = INVALID_SOCKET) noexcept
    {
        if (sock_ != INVALID_SOCKET && sock_ != s) ::closesocket(sock_);
        sock_ = s;
    }
    bool valid() const noexcept { return sock_ != INVALID_SOCKET; }
    explicit operator bool() const noexcept { return valid(); }

private:
    SOCKET sock_ = INVALID_SOCKET;
};

/* 作用域守卫：析构时执行 fn（用于"离开作用域即恢复标志位"这类逻辑）。
 * Scope guard: runs fn on destruction (e.g. restore a flag when leaving scope).
 * 用法 / Usage:
 *     auto guard = vbdmcp::make_scope_guard([&] { flag = false; }); */
template <typename Fn>
class ScopeGuard {
public:
    explicit ScopeGuard(Fn fn) : fn_(std::move(fn)) {}
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&& o) noexcept : fn_(std::move(o.fn_)), armed_(o.armed_)
    {
        o.armed_ = false;
    }
    ~ScopeGuard() { if (armed_) fn_(); }

    /* 提前解除：析构时不再执行 fn。
     * Dismiss early: fn is no longer executed on destruction. */
    void dismiss() noexcept { armed_ = false; }

private:
    Fn   fn_;
    bool armed_ = true;
};

template <typename Fn>
ScopeGuard<Fn> make_scope_guard(Fn fn)
{
    return ScopeGuard<Fn>(std::move(fn));
}

} // namespace vbdmcp

#endif /* VBDMCP_MEM_HPP */
