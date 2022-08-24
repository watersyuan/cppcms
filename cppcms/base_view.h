///////////////////////////////////////////////////////////////////////////////
//                                                                             
//  Copyright (C) 2008-2012  Artyom Beilis (Tonkikh) <artyomtnk@yahoo.com>     
//                                                                             
//  See accompanying file COPYING.TXT file for licensing details.
//
///////////////////////////////////////////////////////////////////////////////
#ifndef CPPCMS_BASE_VIEW_H
#define CPPCMS_BASE_VIEW_H

#include <cppcms/defs.h>

#include <ostream>
#include <sstream>
#include <string>
#include <map>
#include <ctime>
#include <booster/memory_inc.h>

#include <booster/hold_ptr.h>
#include <cppcms/base_content.h>
#include <booster/noncopyable.h>
#include <cppcms/config.h>

namespace cppcms {

///
/// \brief This class is base class for all views (skins) rendered by CppCMS template engine.
/// 
/// Users are not expected to derive from this class or use it directly. CppCMS template compiler
/// create skins that are usable with template engine and each template is derived from the \a base_view
/// class.
///

class CPPCMS_API base_view : booster::noncopyable {
public:
	///
	/// The main rendering function -- render the main HTML page. It is usually overridden in template engine.
	///
	virtual void render();
	virtual ~base_view();
	

protected:

	/// \cond INTERNAL

	base_view(std::ostream &out);
	std::ostream &out();

	/// \endcond

private:
	struct _data;
	booster::hold_ptr<_data> d;

};

} // cppcms


#define CPPCMS_TYPEOF(x) auto


#endif
