///@file
/// Nasal context for testing and executing code
///
// Copyright (C) 2014  Thomas Geymayer <tomgey@gmail.com>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA

#ifndef SG_NASAL_TESTCONTEXT_HXX_
#define SG_NASAL_TESTCONTEXT_HXX_

#include <simgear/nasal/cppbind/NasalCallContext.hxx>

class TestContext:
  public nasal::CallContext
{
  public:
    TestContext():
      CallContext(naNewContext(), naNil(), 0, 0)
    {}

    ~TestContext()
    {
      naFreeContext(c);
    }

    void runGC()
    {
      naFreeContext(c);
      naGC();
      c = naNewContext();
    }

    template<class T>
    T from_str(const std::string& str)
    {
      return from_nasal<T>(to_nasal(str));
    }

    naRef exec(const std::string& code_str, nasal::Me me)
    {
      int err_line = -1;
      naRef code = naParseCode( c, to_nasal("<TextContext::exec>"), 0,
                                (char*)code_str.c_str(), code_str.length(),
                                &err_line );
      if( !naIsCode(code) )
        throw std::runtime_error("Failed to parse code: " + code_str);

      return naCallMethod(code, me, 0, 0, naNil());
    }

    template<class T>
    T exec(const std::string& code)
    {
      return from_nasal<T>(exec(code, naNil()));
    }

    template<class T>
    T convert(const std::string& str)
    {
      return from_nasal<T>(to_nasal(str));
    }
};

#endif /* SG_NASAL_TESTCONTEXT_HXX_ */
