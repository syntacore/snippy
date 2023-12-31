//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// XFAIL: LIBCXX-FREEBSD-FIXME

// UNSUPPORTED: c++03, c++11, c++14, c++17
// UNSUPPORTED: no-localization
// UNSUPPORTED: GCC-ALWAYS_INLINE-FIXME

// TODO FMT This test should not require std::to_chars(floating-point)
// XFAIL: availability-fp_to_chars-missing

// REQUIRES: locale.fr_FR.UTF-8
// REQUIRES: locale.ja_JP.UTF-8

// <chrono>

// class year_month;

// template<class charT, class traits>
//   basic_ostream<charT, traits>&
//     operator<<(basic_ostream<charT, traits>& os, const year_month& ym);

#include <chrono>
#include <cassert>
#include <sstream>

#include "make_string.h"
#include "platform_support.h" // locale name macros
#include "test_macros.h"
#include "assert_macros.h"
#include "concat_macros.h"

#define SV(S) MAKE_STRING_VIEW(CharT, S)

#define TEST_EQUAL(OUT, EXPECTED)                                                                                      \
  TEST_REQUIRE(OUT == EXPECTED,                                                                                        \
               TEST_WRITE_CONCATENATED(                                                                                \
                   "\nExpression      ", #OUT, "\nExpected output ", EXPECTED, "\nActual output   ", OUT, '\n'));

template <class CharT>
static std::basic_string<CharT> stream_c_locale(std::chrono::year_month ym) {
  std::basic_stringstream<CharT> sstr;
  sstr << ym;
  return sstr.str();
}

template <class CharT>
static std::basic_string<CharT> stream_fr_FR_locale(std::chrono::year_month ym) {
  std::basic_stringstream<CharT> sstr;
  const std::locale locale(LOCALE_fr_FR_UTF_8);
  sstr.imbue(locale);
  sstr << ym;
  return sstr.str();
}

template <class CharT>
static std::basic_string<CharT> stream_ja_JP_locale(std::chrono::year_month ym) {
  std::basic_stringstream<CharT> sstr;
  const std::locale locale(LOCALE_ja_JP_UTF_8);
  sstr.imbue(locale);
  sstr << ym;
  return sstr.str();
}

template <class CharT>
static void test() {
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{0}}),
             SV("-32768 is not a valid year/0 is not a valid month"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{1}}),
             SV("-32768 is not a valid year/Jan"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'767}, std::chrono::month{2}}),
             SV("-32767/Feb"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{3}}),
             SV("0000/Mar"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{1970}, std::chrono::month{4}}),
             SV("1970/Apr"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{32'767}, std::chrono::month{5}}),
             SV("32767/May"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{6}}),
             SV("0000/Jun"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{7}}),
             SV("0000/Jul"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{8}}),
             SV("0000/Aug"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{9}}),
             SV("0000/Sep"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{10}}),
             SV("0000/Oct"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{11}}),
             SV("0000/Nov"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{12}}),
             SV("0000/Dec"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{13}}),
             SV("0000/13 is not a valid month"));
  TEST_EQUAL(stream_c_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{255}}),
             SV("-32768 is not a valid year/255 is not a valid month"));

  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{0}}),
             SV("-32768 is not a valid year/0 is not a valid month"));
#if defined(__APPLE__)
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{1}}),
             SV("-32768 is not a valid year/jan"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'767}, std::chrono::month{2}}),
             SV("-32767/fév"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{3}}),
             SV("0000/mar"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{1970}, std::chrono::month{4}}),
             SV("1970/avr"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{32'767}, std::chrono::month{5}}),
             SV("32767/mai"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{6}}),
             SV("0000/jui"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{7}}),
             SV("0000/jul"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{8}}),
             SV("0000/aoû"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{9}}),
             SV("0000/sep"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{10}}),
             SV("0000/oct"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{11}}),
             SV("0000/nov"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{12}}),
             SV("0000/déc"));
#else //  defined(__APPLE__)
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{1}}),
             SV("-32768 is not a valid year/janv."));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'767}, std::chrono::month{2}}),
             SV("-32767/févr."));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{3}}),
             SV("0000/mars"));
#  if defined(_WIN32) || defined(_AIX)
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{1970}, std::chrono::month{4}}),
             SV("1970/avr."));
#  else  // defined(_WIN32) || defined(_AIX)
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{1970}, std::chrono::month{4}}),
             SV("1970/avril"));
#  endif // defined(_WIN32) || defined(_AIX)
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{32'767}, std::chrono::month{5}}),
             SV("32767/mai"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{6}}),
             SV("0000/juin"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{7}}),
             SV("0000/juil."));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{8}}),
             SV("0000/août"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{9}}),
             SV("0000/sept."));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{10}}),
             SV("0000/oct."));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{11}}),
             SV("0000/nov."));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{12}}),
             SV("0000/déc."));
#endif   // defined(__APPLE__)
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{13}}),
             SV("0000/13 is not a valid month"));
  TEST_EQUAL(stream_fr_FR_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{255}}),
             SV("-32768 is not a valid year/255 is not a valid month"));

  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{0}}),
             SV("-32768 is not a valid year/0 is not a valid month"));
#if defined(__APPLE__) || defined(_WIN32)
#  if defined(__APPLE__)
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{1}}),
             SV("-32768 is not a valid year/ 1"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'767}, std::chrono::month{2}}),
             SV("-32767/ 2"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{3}}),
             SV("0000/ 3"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{1970}, std::chrono::month{4}}),
             SV("1970/ 4"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{32'767}, std::chrono::month{5}}),
             SV("32767/ 5"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{6}}),
             SV("0000/ 6"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{7}}),
             SV("0000/ 7"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{8}}),
             SV("0000/ 8"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{9}}),
             SV("0000/ 9"));
#  else  // defined(__APPLE__)
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{1}}),
             SV("-32768 is not a valid year/1"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'767}, std::chrono::month{2}}),
             SV("-32767/2"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{3}}),
             SV("0000/3"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{1970}, std::chrono::month{4}}),
             SV("1970/4"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{32'767}, std::chrono::month{5}}),
             SV("32767/5"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{6}}),
             SV("0000/6"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{7}}),
             SV("0000/7"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{8}}),
             SV("0000/8"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{9}}),
             SV("0000/9"));
#  endif // defined(__APPLE__)
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{10}}),
             SV("0000/10"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{11}}),
             SV("0000/11"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{12}}),
             SV("0000/12"));
#else // defined(__APPLE__) || defined(_WIN32)
#  if defined(_AIX)
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{1}}),
             SV("-32768 is not a valid year/1月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'767}, std::chrono::month{2}}),
             SV("-32767/2月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{3}}),
             SV("0000/3月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{1970}, std::chrono::month{4}}),
             SV("1970/4月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{32'767}, std::chrono::month{5}}),
             SV("32767/5月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{6}}),
             SV("0000/6月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{7}}),
             SV("0000/7月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{8}}),
             SV("0000/8月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{9}}),
             SV("0000/9月"));
#  else  // defined(_AIX)
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{1}}),
             SV("-32768 is not a valid year/ 1月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'767}, std::chrono::month{2}}),
             SV("-32767/ 2月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{3}}),
             SV("0000/ 3月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{1970}, std::chrono::month{4}}),
             SV("1970/ 4月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{32'767}, std::chrono::month{5}}),
             SV("32767/ 5月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{6}}),
             SV("0000/ 6月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{7}}),
             SV("0000/ 7月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{8}}),
             SV("0000/ 8月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{9}}),
             SV("0000/ 9月"));
#  endif // defined(_AIX)
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{10}}),
             SV("0000/10月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{11}}),
             SV("0000/11月"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{12}}),
             SV("0000/12月"));
#endif   // defined(__APPLE__) || defined(_WIN32)
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{0}, std::chrono::month{13}}),
             SV("0000/13 is not a valid month"));
  TEST_EQUAL(stream_ja_JP_locale<CharT>(std::chrono::year_month{std::chrono::year{-32'768}, std::chrono::month{255}}),
             SV("-32768 is not a valid year/255 is not a valid month"));
}

int main(int, char**) {
  test<char>();

#ifndef TEST_HAS_NO_WIDE_CHARACTERS
  test<wchar_t>();
#endif

  return 0;
}
