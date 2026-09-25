#!/bin/sh
# 三段数字版本（major.minor.patch）的校验与比较工具函数。
# 本文件由 upgrade-board.sh 通过“.”加载，不作为独立命令执行。
# 比较时不把版本段转换为 shell 整数，因此前导零和超长数字不会溢出。

# 只接受恰好三个非空数字段，拒绝负号、字母、空段和多余点号。
version_is_valid()
{
    [ "$#" -eq 1 ] || return 1
    version_value=$1

    case "$version_value" in
        ""|.*|*.|*..*|*[!0-9.]*) return 1 ;;
    esac

    version_old_ifs=$IFS
    IFS=.
    set -- $version_value
    IFS=$version_old_ifs

    [ "$#" -eq 3 ] &&
        [ -n "$1" ] && [ -n "$2" ] && [ -n "$3" ]
}

# 去掉前导零，但数值 0 至少保留一个字符。
decimal_normalize()
{
    decimal_value=$1
    while [ "${#decimal_value}" -gt 1 ] &&
          [ "${decimal_value#0}" != "$decimal_value" ]; do
        decimal_value=${decimal_value#0}
    done
    printf '%s\n' "$decimal_value"
}

# 以“长度→字节序”比较任意长的非负十进制数字字符串。
# 输出 -1、0 或 1，分别表示左值小于、等于或大于右值。
decimal_compare()
{
    decimal_left=$(decimal_normalize "$1")
    decimal_right=$(decimal_normalize "$2")

    if [ "${#decimal_left}" -lt "${#decimal_right}" ]; then
        printf '%s\n' -1
        return 0
    fi
    if [ "${#decimal_left}" -gt "${#decimal_right}" ]; then
        printf '%s\n' 1
        return 0
    fi
    if [ "$decimal_left" = "$decimal_right" ]; then
        printf '%s\n' 0
        return 0
    fi

    decimal_first=$(printf '%s\n%s\n' "$decimal_left" "$decimal_right" |
        LC_ALL=C sort | sed -n '1p')
    if [ "$decimal_first" = "$decimal_left" ]; then
        printf '%s\n' -1
    else
        printf '%s\n' 1
    fi
}

# 校验并拆分版本，通过 VERSION_MAJOR/MINOR/PATCH 返回三段字符串。
version_split()
{
    version_is_valid "$1" || return 1
    version_old_ifs=$IFS
    IFS=.
    set -- $1
    IFS=$version_old_ifs
    VERSION_MAJOR=$1
    VERSION_MINOR=$2
    VERSION_PATCH=$3
}

# 按 major、minor、patch 的顺序逐段比较两个版本。
version_compare()
{
    [ "$#" -eq 2 ] || return 1
    version_split "$1" || return 1
    version_left_major=$VERSION_MAJOR
    version_left_minor=$VERSION_MINOR
    version_left_patch=$VERSION_PATCH

    version_split "$2" || return 1

    for version_pair in \
        "$version_left_major:$VERSION_MAJOR" \
        "$version_left_minor:$VERSION_MINOR" \
        "$version_left_patch:$VERSION_PATCH"; do
        version_left=${version_pair%%:*}
        version_right=${version_pair#*:}
        version_result=$(decimal_compare "$version_left" "$version_right")
        if [ "$version_result" != 0 ]; then
            printf '%s\n' "$version_result"
            return 0
        fi
    done

    printf '%s\n' 0
}
