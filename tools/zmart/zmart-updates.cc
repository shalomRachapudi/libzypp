
#include <iostream>
#include <fstream>
#include <sstream>
#include <streambuf>

#include <zypp/Patch.h>
#include <zypp/base/LogControl.h>
#include <zypp/base/Logger.h>
#include <zypp/base/String.h>
#include <zypp/Locale.h>
#include <zypp/ZYpp.h>
#include <zypp/ZYppFactory.h>
#include <zypp/zypp_detail/ZYppReadOnlyHack.h>
#include <zypp/SourceManager.h>
#include <zypp/base/Logger.h>
#include <zypp/Digest.h>
#include <zypp/target/store/xml_escape_parser.hpp>

#include "zmart.h"
#include "zmart-updates.h"

using namespace std;
using namespace zypp;

extern ZYpp::Ptr God;
extern RuntimeData gData;
extern Settings gSettings;

string read_old_token()
{
  string buffer;
  string token;
  std::ifstream is(TOKEN_FILE);
  if ( is.good() )
  {
    while(is && !is.eof())
    {
      getline(is, buffer);
      token += buffer;
    }
    is.close();
  }
  return token;
}

void save_token( const std::string &token )
{
  std::ofstream os(TOKEN_FILE);
  if ( os.good() )
  {
    os << token << endl;;
  }
  os.close();
}

static std::string xml_escape( const std::string &text )
{
  iobind::parser::xml_escape_parser parser;
  return parser.escape(text);
}

void render_error(  std::ostream &out, const std::string &reason )
{
  out << "<update-status op=\"error\">" << std::endl;
    out << "<error>" << reason << "</error>" << std::endl;
  out << "</update-status>" << std::endl;
}

void render_unchanged(  std::ostream &out, const std::string &token )
{
  out << "<update-status op=\"unchanged\">" << std::endl;
  //  out << " <metadata token=\"" << token << "\"/>" << std::endl;
  out << "</update-status>" << std::endl;
}

void render_result( std::ostream &out, const zypp::ResPool &pool)
{
  int count = 0;
  int security_count = 0;
  
  out << "<?xml version='1.0'?>" << std::endl;
  out << "<update-status op=\"success\">" << std::endl;
  //out << " <metadata token=\"" << token << "\"/>" << std::endl;
  out << " <update-sources>" << std::endl;
  for ( std::list<Source_Ref>::const_iterator it = gData.sources.begin(); it != gData.sources.end(); ++it )
  {
    out << "  <source url=\"" << it->url() << "\" alias=\"" << it->alias() << "\"/>" << std::endl;
  }
  out << " </update-sources>" << std::endl;
  out << " <update-list>" << std::endl;
  for ( ResPool::byKind_iterator it = pool.byKindBegin<Patch>(); it != pool.byKindEnd<Patch>(); ++it )
  {
    Resolvable::constPtr res = it->resolvable();
    Patch::constPtr patch = asKind<Patch>(res);
    MIL << patch->name() << " " << patch->edition() << " " << "[" << patch->category() << "]" << ( it->status().isNeeded() ? " [needed]" : " [unneeded]" )<< std::endl;
    if ( it->status().isNeeded() )
    {
      out << " <update category=\"" << patch ->category() << "\" name=\"" << patch->name() << "\" edition=\"" << patch->edition() << "\"" << ">" << std::endl;
      out << " <summary>" << xml_escape(patch->summary()) << "</summary>" << endl;
      out << " <description>" << xml_escape(patch->description()) << "</description>" << endl;
      if ( patch->source() != Source_Ref::noSource )
        out << "<source url=\"" << patch->source().url() << "\" alias=\"" << patch->source().alias() << "\"/>" << std::endl;
      out << "</update>" << endl;
      
      count++;
      if (patch->category() == "security")
        security_count++;
    }
  }
  out << " </update-list>" << std::endl;
  out << " <update-summary total=\"" << count << "\" security=\"" << security_count << "\"/>" << std::endl;
  out << "</update-status>" << std::endl;
}
