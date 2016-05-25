/*
 *  Copyright (C) 2004-2016 Savoir-faire Linux Inc.
 *
 *  Author: Olivier Grégoire <olivier.gregoire@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */
 #ifndef SMARTOOLS_H_
 #define SMARTOOLS_H_

 #include <chrono>

 #include "threadloop.h"
 #include "manager.h"

 namespace ring {
 class Smartools{
    public:
      Smartools();
      void start();
      void stop();

    private:
      static constexpr auto SLEEP_TIME = std::chrono::milliseconds(500);
      void process();
      ThreadLoop loop_; // as to be last member
  };


} //ring namespace
#endif //smartools.h
