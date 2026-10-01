#pragma once

#include <QColor>
#include "Enums.h"

namespace Miryu {

// Marker colors for queued actions, shared by the package-list row marker
// (PackageView) and the queue page (QueueView):
//   Reinstall = blue, Downgrade = yellow, Remove = red,
//   Install = green, Update = orange, DistroSync = blue.
inline QColor todoColor(PackageTodo todo)
{
    switch (todo) {
    case PackageTodo::Install:    return QColor(46, 160, 67);   // green
    case PackageTodo::Update:     return QColor(255, 140, 0);   // orange
    case PackageTodo::Remove:     return QColor(192, 28, 40);   // red
    case PackageTodo::Downgrade:  return QColor(255, 185, 0);   // yellow
    case PackageTodo::Reinstall:  return QColor(0, 114, 178);   // blue
    case PackageTodo::DistroSync: return QColor(0, 114, 178);   // blue
    case PackageTodo::None:       return QColor(120, 120, 120); // gray
    }
    return QColor(120, 120, 120);
}

}
