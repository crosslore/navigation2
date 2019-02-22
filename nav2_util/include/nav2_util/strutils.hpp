// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NAV2_UTIL__STRUTILS_HPP_
#define NAV2_UTIL__STRUTILS_HPP_

#include <string>

namespace nav2_util
{

class strutils
{
public:
  static std::string stripLeadingSlash(const std::string & in);
};

std::string strutils::stripLeadingSlash(const std::string & in)
{
  std::string out = in;

  if ((!in.empty()) && (in[0] == '/')) {
    out.erase(0, 1);
  }

  return out;
}

}  // namespace nav2_util

#endif  // NAV2_UTIL__STRUTILS_HPP_
