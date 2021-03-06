#ifndef _CARTOONLINETILEDATASOURCE_I
#define _CARTOONLINETILEDATASOURCE_I

%module(directors="1") CartoOnlineTileDataSource

#ifdef _CARTO_CUSTOM_BASEMAP_SUPPORT

!proxy_imports(carto::CartoOnlineTileDataSource, core.MapTile, core.StringMap, datasources.TileDataSource, datasources.components.TileData)

%{
#include "datasources/CartoOnlineTileDataSource.h"
#include "components/Exceptions.h"
#include <memory>
%}

%include <std_shared_ptr.i>
%include <std_string.i>
%include <cartoswig.i>

%import "datasources/TileDataSource.i"

!polymorphic_shared_ptr(carto::CartoOnlineTileDataSource, datasources.CartoOnlineTileDataSource)

%std_exceptions(carto::CartoOnlineTileDataSource::CartoOnlineTileDataSource)

%ignore carto::CartoOnlineTileDataSource::buildTileURL;
%ignore carto::CartoOnlineTileDataSource::loadTileURLs;
%ignore carto::CartoOnlineTileDataSource::loadOnlineTile;

%feature("director") carto::CartoOnlineTileDataSource;

%include "datasources/CartoOnlineTileDataSource.h"

#endif

#endif
