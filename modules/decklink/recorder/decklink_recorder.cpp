/*
* This file is part of TVP's fork of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
*/

#include "../StdAfx.h"
#include "decklink_recorder.h"

#include <core/recorder.h>

#include "../interop/DeckLinkAPI_h.h"



namespace caspar {
	namespace decklink {

		class decklink_recorder : public core::recorder 
		{
		private:
			int				index_;

		public:
			decklink_recorder(int index, boost::property_tree::wptree ptree) :
				index_(index)
			{
			
			}
			virtual boost::property_tree::wptree info()const {
				return boost::property_tree::wptree();
			}

			virtual int index() const {
				return index_;
			}

		};


		safe_ptr<core::recorder> create_recorder(int index, const boost::property_tree::wptree& ptree)
		{
			return make_safe<decklink_recorder>(index, ptree);
		}

	}
}