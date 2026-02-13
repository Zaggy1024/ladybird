/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

struct NonTriviallyCopyable {
    NonTriviallyCopyable() = default;
    NonTriviallyCopyable(NonTriviallyCopyable const&) { }
    NonTriviallyCopyable& operator=(NonTriviallyCopyable const&) { return *this; }
};

struct TriviallyCopyable {
    int x;
};

void test()
{
    NonTriviallyCopyable ntc;
    NonTriviallyCopyable const& ntc_cref = ntc;
    NonTriviallyCopyable const ntc_const;
    TriviallyCopyable const& tc_cref = {};
    int x = 0;

    // OK: non-const source, lambda move constructor can move captures
    (void)[ntc]() {
        (void)ntc;
    };

    // OK: captured by reference, no copy
    (void)[&ntc]() {
        (void)ntc;
    };

    // OK: mutable lambda with non-const source
    (void)[ntc]() mutable {
        (void)ntc;
    };

    // Error: const ref source, lambda member is const NonTriviallyCopyable,
    // move constructor falls back to copy
    // expected-error@+2 {{non-trivially-copyable type 'NonTriviallyCopyable' is captured by value as a const copy in a lambda}}
    // expected-note@+1 {{capture by reference, remove const from the source declaration, or use a capture initializer (e.g. [name = name])}}
    (void)[ntc_cref]() {
        (void)ntc_cref;
    };

    // Error: same problem even in mutable lambda - member type is still const
    // expected-error@+2 {{non-trivially-copyable type 'NonTriviallyCopyable' is captured by value as a const copy in a lambda}}
    // expected-note@+1 {{capture by reference, remove const from the source declaration, or use a capture initializer (e.g. [name = name])}}
    (void)[ntc_cref]() mutable {
        (void)ntc_cref;
    };

    // Error: const variable source, same issue
    // expected-error@+2 {{non-trivially-copyable type 'NonTriviallyCopyable' is captured by value as a const copy in a lambda}}
    // expected-note@+1 {{capture by reference, remove const from the source declaration, or use a capture initializer (e.g. [name = name])}}
    (void)[ntc_const]() mutable {
        (void)ntc_const;
    };

    // OK: capture initializer strips const, member is non-const
    (void)[ntc_cref = ntc_cref]() {
        (void)ntc_cref;
    };

    // OK: capture initializer from const variable
    (void)[ntc_const = ntc_const]() {
        (void)ntc_const;
    };

    // OK: const ref captured by reference, no copy
    (void)[&ntc_cref]() {
        (void)ntc_cref;
    };

    // OK: trivially copyable type, even if const
    (void)[tc_cref]() {
        (void)tc_cref;
    };

    // OK: primitive type
    (void)[x]() {
        (void)x;
    };
}
