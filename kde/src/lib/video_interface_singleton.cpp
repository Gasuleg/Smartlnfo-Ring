/************************************************************************************
 *   Copyright (C) 2012 by Savoir-Faire Linux                                       *
 *   Author : Emmanuel Lepage Vallee <emmanuel.lepage@savoirfairelinux.com>         *
 *                                                                                  *
 *   This library is free software; you can redistribute it and/or                  *
 *   modify it under the terms of the GNU Lesser General Public                     *
 *   License as published by the Free Software Foundation; either                   *
 *   version 2.1 of the License, or (at your option) any later version.             *
 *                                                                                  *
 *   This library is distributed in the hope that it will be useful,                *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of                 *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU              *
 *   Lesser General Public License for more details.                                *
 *                                                                                  *
 *   You should have received a copy of the GNU Lesser General Public               *
 *   License along with this library; if not, write to the Free Software            *
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA *
 ***********************************************************************************/

#include "video_interface_singleton.h"

VideoInterface* VideoInterfaceSingleton::interface = NULL;

VideoInterface& VideoInterfaceSingleton::getInstance()
{
   if (!interface)
      interface = new VideoInterface("org.sflphone.SFLphone", "/org/sflphone/SFLphone/VideoControls", QDBusConnection::sessionBus());
   
   if(!interface->connection().isConnected()) {
      throw "Error : sflphoned not connected. Service " + interface->service() + " not connected. From instance interface.";
   }
   return *interface;
}