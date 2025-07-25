/******************************************************************************
 *
 * Project:  OGR Core SWIG Interface declarations.
 * Purpose:  OGR declarations.
 * Author:   Howard Butler, hobu@iastate.edu
 *
 ******************************************************************************
 * Copyright (c) 2005, Howard Butler
 *
 * SPDX-License-Identifier: MIT
 *****************************************************************************/

#ifdef SWIGPYTHON
%nothread;
%inline %{
typedef void* VoidPtrAsLong;
%}
#endif

#ifndef FROM_GDAL_I
%include "exception.i"
#endif

#if defined(SWIGCSHARP)
%module Ogr
#elif defined(SWIGPYTHON)
%module (package="osgeo") ogr
#else
%module ogr
#endif

#ifndef FROM_GDAL_I
%inline %{
typedef char retStringAndCPLFree;
%}
#endif

#ifdef SWIGJAVA
%inline %{
typedef const char StringAsByteArray;
%}
#endif

#ifdef SWIGCSHARP
%include swig_csharp_extensions.i
#endif

#ifndef SWIGJAVA
%feature("compactdefaultargs");
#endif

%feature("autodoc");

/************************************************************************/
/*                         Enumerated types                             */
/************************************************************************/

#ifndef SWIGCSHARP
typedef int OGRwkbByteOrder;
typedef int OGRwkbGeometryType;
typedef int OGRFieldType;
typedef int OGRFieldSubType;
typedef int OGRJustification;
typedef int OGRFieldDomainType;
typedef int OGRFieldDomainSplitPolicy;
typedef int OGRFieldDomainMergePolicy;
#else
%rename (wkbByteOrder) OGRwkbByteOrder;
typedef enum
{
    wkbXDR = 0,         /* MSB/Sun/Motorola: Most Significant Byte First   */
    wkbNDR = 1          /* LSB/Intel/Vax: Least Significant Byte First      */
} OGRwkbByteOrder;

%rename (wkbGeometryType) OGRwkbGeometryType;
typedef enum
{
    wkbUnknown = 0,             /* non-standard */
    wkbPoint = 1,               /* rest are standard WKB type codes */
    wkbLineString = 2,
    wkbPolygon = 3,
    wkbMultiPoint = 4,
    wkbMultiLineString = 5,
    wkbMultiPolygon = 6,
    wkbGeometryCollection = 7,

    wkbCircularString = 8,  /**< one or more circular arc segments connected end to end,
                             *   ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbCompoundCurve = 9,   /**< sequence of contiguous curves, ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbCurvePolygon = 10,   /**< planar surface, defined by 1 exterior boundary
                             *   and zero or more interior boundaries, that are curves.
                             *    ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbMultiCurve = 11,     /**< GeometryCollection of Curves, ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbMultiSurface = 12,   /**< GeometryCollection of Surfaces, ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbCurve = 13,          /**< Curve (abstract type). ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbSurface = 14,        /**< Surface (abstract type). ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbPolyhedralSurface = 15,/**< a contiguous collection of polygons, which share common boundary segments,
                               *   ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbTIN = 16,              /**< a PolyhedralSurface consisting only of Triangle patches
                               *    ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbTriangle = 17,       /**< A Triangle is a polygon with 3 distinct, non-collinear vertices and no
                                 interior boundary. GDAL &gt;= 2.2 */

    wkbNone = 100,          /**< non-standard, for pure attribute records */
    wkbLinearRing = 101,    /**< non-standard, just for createGeometry() */

    wkbCircularStringZ = 1008,  /**< wkbCircularString with Z component. ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbCompoundCurveZ = 1009,   /**< wkbCompoundCurve with Z component. ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbCurvePolygonZ = 1010,    /**< wkbCurvePolygon with Z component. ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbMultiCurveZ = 1011,      /**< wkbMultiCurve with Z component. ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbMultiSurfaceZ = 1012,    /**< wkbMultiSurface with Z component. ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbCurveZ = 1013,           /**< wkbCurve with Z component. ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbSurfaceZ = 1014,         /**< wkbSurface with Z component. ISO SQL/MM Part 3. GDAL &gt;= 2.0 */
    wkbPolyhedralSurfaceZ = 1015,  /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbTINZ = 1016,                /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbTriangleZ = 1017,         /**< wkbTriangle with Z component. GDAL &gt;= 2.2 */

    wkbPointM = 2001,              /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbLineStringM = 2002,         /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbPolygonM = 2003,            /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiPointM = 2004,         /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiLineStringM = 2005,    /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiPolygonM = 2006,       /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbGeometryCollectionM = 2007, /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbCircularStringM = 2008,     /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbCompoundCurveM = 2009,      /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbCurvePolygonM = 2010,       /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiCurveM = 2011,         /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiSurfaceM = 2012,       /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbCurveM = 2013,              /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbSurfaceM = 2014,            /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbPolyhedralSurfaceM = 2015,  /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbTINM = 2016,                /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbTriangleM = 2017,            /**<  GDAL &gt;= 2.2 */

    wkbPointZM = 3001,              /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbLineStringZM = 3002,         /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbPolygonZM = 3003,            /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiPointZM = 3004,         /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiLineStringZM = 3005,    /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiPolygonZM = 3006,       /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbGeometryCollectionZM = 3007, /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbCircularStringZM = 3008,     /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbCompoundCurveZM = 3009,      /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbCurvePolygonZM = 3010,       /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiCurveZM = 3011,         /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbMultiSurfaceZM = 3012,       /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbCurveZM = 3013,              /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbSurfaceZM = 3014,            /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbPolyhedralSurfaceZM = 3015,  /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbTINZM = 3016,                /**< ISO SQL/MM Part 3. GDAL &gt;= 2.1 */
    wkbTriangleZM = 3017,           /**<  GDAL &gt;= 2.2 */

    wkbPoint25D = -2147483647,   /* 2.5D extensions as per 99-402 */
    wkbLineString25D = -2147483646,
    wkbPolygon25D = -2147483645,
    wkbMultiPoint25D = -2147483644,
    wkbMultiLineString25D = -2147483643,
    wkbMultiPolygon25D = -2147483642,
    wkbGeometryCollection25D = -2147483641
} OGRwkbGeometryType;

%rename (FieldType) OGRFieldType;
typedef enum
{
  /** Simple 32bit integer */                   OFTInteger = 0,
  /** List of 32bit integers */                 OFTIntegerList = 1,
  /** Double Precision floating point */        OFTReal = 2,
  /** List of doubles */                        OFTRealList = 3,
  /** String of ASCII chars */                  OFTString = 4,
  /** Array of strings */                       OFTStringList = 5,
  /** Double byte string (unsupported) */       OFTWideString = 6,
  /** List of wide strings (unsupported) */     OFTWideStringList = 7,
  /** Raw Binary data */                        OFTBinary = 8,
  /** Date */                                   OFTDate = 9,
  /** Time */                                   OFTTime = 10,
  /** Date and Time */                          OFTDateTime = 11,
  /** Single 64bit integer */                   OFTInteger64 = 12,
  /** List of 64bit integers */                 OFTInteger64List = 13
} OGRFieldType;

%rename (FieldSubType) OGRFieldSubType;
typedef enum
{
    /** No subtype. This is the default value */        OFSTNone = 0,
    /** Boolean integer. Only valid for OFTInteger
        and OFTIntegerList.*/                           OFSTBoolean = 1,
    /** Signed 16-bit integer. Only valid for OFTInteger and OFTIntegerList. */
                                                        OFSTInt16 = 2,
    /** Single precision (32 bit) floating point. Only valid for OFTReal and OFTRealList. */
                                                        OFSTFloat32 = 3,
    /** JSON content. Only valid for OFTString.
     * @since GDAL 2.4
     */
                                                        OFSTJSON = 4,
    /** UUID string representation. Only valid for OFTString.
     * @since GDAL 3.3
     */
                                                        OFSTUUID = 5,
} OGRFieldSubType;


%rename (Justification) OGRJustification;
typedef enum
{
    OJUndefined = 0,
    OJLeft = 1,
    OJRight = 2
} OGRJustification;

%rename (FieldDomainType) OGRFieldDomainType;
typedef enum
{
    /** Coded */
    OFDT_CODED = 0,
    /** Range (min/max) */
    OFDT_RANGE = 1,
    /** Glob (used by GeoPackage) */
    OFDT_GLOB = 2
} OGRFieldDomainType;

%rename (FieldDomainSplitPolicy) OGRFieldDomainSplitPolicy;
typedef enum
{
    /** Default value */
    OFDSP_DEFAULT_VALUE,
    /** Duplicate */
    OFDSP_DUPLICATE,
    /** New values are computed by the ratio of their area/length compared to the area/length of the original feature */
    OFDSP_GEOMETRY_RATIO
} OGRFieldDomainSplitPolicy;

%rename (FieldDomainMergePolicy) OGRFieldDomainMergePolicy;
typedef enum
{
    /** Default value */
    OFDMP_DEFAULT_VALUE,
    /** Sum */
    OFDMP_SUM,
    /** New values are computed as the weighted average of the source values. */
    OFDMP_GEOMETRY_WEIGHTED
} OGRFieldDomainMergePolicy;


#endif


%{
#include <iostream>
using namespace std;

#define CPL_SUPRESS_CPLUSPLUS

#include "gdal.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "cpl_port.h"
#include "cpl_string.h"
#include "ogr_srs_api.h"
#include "ogr_recordbatch.h"
#include "ogr_p.h"

#define FIELD_INDEX_ERROR_TMPL "Invalid field index: '%i'"
#define FIELD_NAME_ERROR_TMPL "Invalid field name: '%s'"

typedef void GDALMajorObjectShadow;
typedef void GDALDatasetShadow;

#ifdef DEBUG
typedef struct OGRSpatialReferenceHS OSRSpatialReferenceShadow;
typedef struct OGRDriverHS OGRDriverShadow;
typedef struct OGRDataSourceHS OGRDataSourceShadow;
typedef struct OGRLayerHS OGRLayerShadow;
typedef struct OGRFeatureHS OGRFeatureShadow;
typedef struct OGRFeatureDefnHS OGRFeatureDefnShadow;
typedef struct OGRGeometryHS OGRGeometryShadow;
typedef struct OGRCoordinateTransformationHS OSRCoordinateTransformationShadow;
typedef struct OGRFieldDefnHS OGRFieldDefnShadow;
#else
typedef void OSRSpatialReferenceShadow;
typedef void OGRDriverShadow;
typedef void OGRDataSourceShadow;
typedef void OGRLayerShadow;
typedef void OGRFeatureShadow;
typedef void OGRFeatureDefnShadow;
typedef void OGRGeometryShadow;
typedef void OSRCoordinateTransformationShadow;
typedef void OGRFieldDefnShadow;
#endif

typedef struct OGRStyleTableHS OGRStyleTableShadow;
typedef struct OGRGeomFieldDefnHS OGRGeomFieldDefnShadow;
typedef struct OGRGeomTransformer OGRGeomTransformerShadow;
typedef struct _OGRPreparedGeometry OGRPreparedGeometryShadow;
typedef struct OGRFieldDomainHS OGRFieldDomainShadow;
typedef struct OGRGeomCoordinatePrecision OGRGeomCoordinatePrecisionShadow;
%}

#ifdef SWIGJAVA
%{
typedef void retGetPoints;
%}
#endif

#ifndef SWIGCSHARP
#ifdef SWIGJAVA
%javaconst(1);
#endif
/* Interface constant added for GDAL 1.7.0 */
%constant wkb25DBit = 0x80000000;

/* typo : deprecated */
%constant wkb25Bit = 0x80000000;

%constant wkbUnknown = 0;

%constant wkbPoint = 1;
%constant wkbLineString = 2;
%constant wkbPolygon = 3;
%constant wkbMultiPoint = 4;
%constant wkbMultiLineString = 5;
%constant wkbMultiPolygon = 6;
%constant wkbGeometryCollection = 7;

%constant wkbCircularString = 8;
%constant wkbCompoundCurve = 9;
%constant wkbCurvePolygon = 10;
%constant wkbMultiCurve = 11;
%constant wkbMultiSurface = 12;
%constant wkbCurve = 13;
%constant wkbSurface = 14;
%constant wkbPolyhedralSurface = 15;
%constant wkbTIN = 16;
%constant wkbTriangle = 17;

%constant wkbNone = 100;
%constant wkbLinearRing = 101;

%constant wkbCircularStringZ = 1008;
%constant wkbCompoundCurveZ = 1009;
%constant wkbCurvePolygonZ = 1010;
%constant wkbMultiCurveZ = 1011;
%constant wkbMultiSurfaceZ = 1012;
%constant wkbCurveZ = 1013;
%constant wkbSurfaceZ = 1014;
%constant wkbPolyhedralSurfaceZ = 1015;
%constant wkbTINZ = 1016;
%constant wkbTriangleZ = 1017;

%constant wkbPointM = 2001;
%constant wkbLineStringM = 2002;
%constant wkbPolygonM = 2003;
%constant wkbMultiPointM = 2004;
%constant wkbMultiLineStringM = 2005;
%constant wkbMultiPolygonM = 2006;
%constant wkbGeometryCollectionM = 2007;
%constant wkbCircularStringM = 2008;
%constant wkbCompoundCurveM = 2009;
%constant wkbCurvePolygonM = 2010;
%constant wkbMultiCurveM = 2011;
%constant wkbMultiSurfaceM = 2012;
%constant wkbCurveM = 2013;
%constant wkbSurfaceM = 2014;
%constant wkbPolyhedralSurfaceM = 2015;
%constant wkbTINM = 2016;
%constant wkbTriangleM = 2017;

%constant wkbPointZM = 3001;
%constant wkbLineStringZM = 3002;
%constant wkbPolygonZM = 3003;
%constant wkbMultiPointZM = 3004;
%constant wkbMultiLineStringZM = 3005;
%constant wkbMultiPolygonZM = 3006;
%constant wkbGeometryCollectionZM = 3007;
%constant wkbCircularStringZM = 3008;
%constant wkbCompoundCurveZM = 3009;
%constant wkbCurvePolygonZM = 3010;
%constant wkbMultiCurveZM = 3011;
%constant wkbMultiSurfaceZM = 3012;
%constant wkbCurveZM = 3013;
%constant wkbSurfaceZM = 3014;
%constant wkbPolyhedralSurfaceZM = 3015;
%constant wkbTINZM = 3016;
%constant wkbTriangleZM = 3017;

%constant wkbPoint25D =              0x80000001;
%constant wkbLineString25D =         0x80000002;
%constant wkbPolygon25D =            0x80000003;
%constant wkbMultiPoint25D =         0x80000004;
%constant wkbMultiLineString25D =    0x80000005;
%constant wkbMultiPolygon25D =       0x80000006;
%constant wkbGeometryCollection25D = 0x80000007;

%constant OFTInteger = 0;
%constant OFTIntegerList= 1;
%constant OFTReal = 2;
%constant OFTRealList = 3;
%constant OFTString = 4;
%constant OFTStringList = 5;
%constant OFTWideString = 6;
%constant OFTWideStringList = 7;
%constant OFTBinary = 8;
%constant OFTDate = 9;
%constant OFTTime = 10;
%constant OFTDateTime = 11;
%constant OFTInteger64 = 12;
%constant OFTInteger64List = 13;

%constant OFSTNone = 0;
%constant OFSTBoolean = 1;
%constant OFSTInt16 = 2;
%constant OFSTFloat32 = 3;
%constant OFSTJSON = 4;
%constant OFSTUUID = 5;

%constant OJUndefined = 0;
%constant OJLeft = 1;
%constant OJRight = 2;

%constant OFDT_CODED = 0;
%constant OFDT_RANGE = 1;
%constant OFDT_GLOB = 2;

%constant OFDSP_DEFAULT_VALUE = 0;
%constant OFDSP_DUPLICATE = 1;
%constant OFDSP_GEOMETRY_RATIO = 2;

%constant OFDMP_DEFAULT_VALUE = 0;
%constant OFDMP_SUM = 1;
%constant OFDMP_GEOMETRY_WEIGHTED = 2;

%constant wkbXDR = 0;
%constant wkbNDR = 1;

%constant NullFID = -1;

%constant ALTER_NAME_FLAG = 1;
%constant ALTER_TYPE_FLAG = 2;
%constant ALTER_WIDTH_PRECISION_FLAG = 4;
%constant ALTER_NULLABLE_FLAG = 8;
%constant ALTER__FLAG = 8;
%constant ALTER_DEFAULT_FLAG = 16;
%constant ALTER_UNIQUE_FLAG = 32;
%constant ALTER_DOMAIN_FLAG = 64;
%constant ALTER_ALTERNATIVE_NAME_FLAG = 128;
%constant ALTER_COMMENT_FLAG = 256;
%constant ALTER_ALL_FLAG = 1 + 2 + 4 + 8 + 16 + 32 + 64 + 128 + 256;

%constant ALTER_GEOM_FIELD_DEFN_NAME_FLAG = 4096;
%constant ALTER_GEOM_FIELD_DEFN_TYPE_FLAG = 8192;
%constant ALTER_GEOM_FIELD_DEFN_NULLABLE_FLAG = 16384;
%constant ALTER_GEOM_FIELD_DEFN_SRS_FLAG = 32768;
%constant ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG = 65536;
%constant ALTER_GEOM_FIELD_DEFN_ALL_FLAG = 4096 + 8192 + 16384 + 32768 + 65536;

%constant F_VAL_NULL= 0x00000001; /**< Validate that fields respect not-null constraints */
%constant F_VAL_GEOM_TYPE = 0x00000002; /**< Validate that geometries respect geometry column type */
%constant F_VAL_WIDTH = 0x00000004; /**< Validate that (string) fields respect field width */
%constant F_VAL_ALLOW_NULL_WHEN_DEFAULT = 0x00000008; /***<Allow fields that are null when there's an associated default value. */
%constant F_VAL_ALL = 0xFFFFFFFF; /**< Enable all validation tests */

%constant TZFLAG_UNKNOWN = 0;
%constant TZFLAG_LOCALTIME = 1;
%constant TZFLAG_MIXED_TZ = 2;
%constant TZFLAG_UTC = 100;

/** Flag for OGR_L_GetGeometryTypes() indicating that
 * OGRGeometryTypeCounter::nCount value is not needed */
%constant GGT_COUNT_NOT_NEEDED = 0x1;

/** Flag for OGR_L_GetGeometryTypes() indicating that iteration might stop as
 * sooon as 2 distinct geometry types are found. */
%constant GGT_STOP_IF_MIXED = 0x2;

/** Flag for OGR_L_GetGeometryTypes() indicating that a GeometryCollectionZ
 * whose first subgeometry is a TinZ should be reported as TinZ */
%constant GGT_GEOMCOLLECTIONZ_TINZ = 0x4;

%constant char *OLCRandomRead          = "RandomRead";
%constant char *OLCSequentialWrite     = "SequentialWrite";
%constant char *OLCRandomWrite         = "RandomWrite";
%constant char *OLCFastSpatialFilter   = "FastSpatialFilter";
%constant char *OLCFastFeatureCount    = "FastFeatureCount";
%constant char *OLCFastGetExtent       = "FastGetExtent";
%constant char *OLCFastGetExtent3D     = "FastGetExtent3D";
%constant char *OLCCreateField         = "CreateField";
%constant char *OLCDeleteField         = "DeleteField";
%constant char *OLCReorderFields       = "ReorderFields";
%constant char *OLCAlterFieldDefn      = "AlterFieldDefn";
%constant char *OLCAlterGeomFieldDefn  = "AlterGeomFieldDefn";
%constant char *OLCTransactions        = "Transactions";
%constant char *OLCDeleteFeature       = "DeleteFeature";
%constant char *OLCUpsertFeature       = "UpsertFeature";
%constant char *OLCUpdateFeature       = "UpdateFeature";
%constant char *OLCFastSetNextByIndex  = "FastSetNextByIndex";
%constant char *OLCStringsAsUTF8       = "StringsAsUTF8";
%constant char *OLCIgnoreFields        = "IgnoreFields";
%constant char *OLCCreateGeomField     = "CreateGeomField";
%constant char *OLCCurveGeometries     = "CurveGeometries";
%constant char *OLCMeasuredGeometries  = "MeasuredGeometries";
%constant char *OLCZGeometries         = "ZGeometries";
%constant char *OLCRename              = "Rename";
%constant char *OLCFastGetArrowStream  = "FastGetArrowStream";
%constant char *OLCFastWriteArrowBatch = "FastWriteArrowBatch";

%constant char *ODsCCreateLayer        = "CreateLayer";
%constant char *ODsCDeleteLayer        = "DeleteLayer";
%constant char *ODsCCreateGeomFieldAfterCreateLayer  = "CreateGeomFieldAfterCreateLayer";
%constant char *ODsCCurveGeometries    = "CurveGeometries";
%constant char *ODsCTransactions       = "Transactions";
%constant char *ODsCEmulatedTransactions = "EmulatedTransactions";
%constant char *ODsCMeasuredGeometries = "MeasuredGeometries";
%constant char *ODsCZGeometries        = "ZGeometries";
%constant char *ODsCRandomLayerRead    = "RandomLayerRead";
/* Note the unfortunate trailing space at the end of the string */
%constant char *ODsCRandomLayerWrite   = "RandomLayerWrite ";
%constant char *ODsCAddFieldDomain     = "AddFieldDomain";
%constant char *ODsCDeleteFieldDomain  = "DeleteFieldDomain";
%constant char *ODsCUpdateFieldDomain  = "UpdateFieldDomain";

%constant char *ODrCCreateDataSource   = "CreateDataSource";
%constant char *ODrCDeleteDataSource   = "DeleteDataSource";

%constant char *OLMD_FID64             = "OLMD_FID64";

%constant int GEOS_PREC_NO_TOPO = 1;
%constant int GEOS_PREC_KEEP_COLLAPSED = 2;

#else
typedef int OGRErr;
typedef int CPLErr;

#define wkb25DBit 0x80000000
#define ogrZMarker 0x21125711

#define OGRNullFID            -1
#define OGRUnsetMarker        -21121

#define OLCRandomRead          "RandomRead"
#define OLCSequentialWrite     "SequentialWrite"
#define OLCRandomWrite         "RandomWrite"
#define OLCFastSpatialFilter   "FastSpatialFilter"
#define OLCFastFeatureCount    "FastFeatureCount"
#define OLCFastGetExtent       "FastGetExtent"
#define OLCFastGetExtent3D     "FastGetExtent3D"
#define OLCCreateField         "CreateField"
#define OLCDeleteField         "DeleteField"
#define OLCReorderFields       "ReorderFields"
#define OLCAlterFieldDefn      "AlterFieldDefn"
#define OLCAlterGeomFieldDefn  "AlterGeomFieldDefn"
#define OLCTransactions        "Transactions"
#define OLCDeleteFeature       "DeleteFeature"
#define OLCUpsertFeature       "UpsertFeature"
#define OLCUpdateFeature       "UpdateFeature"
#define OLCFastSetNextByIndex  "FastSetNextByIndex"
#define OLCStringsAsUTF8       "StringsAsUTF8"
#define OLCCreateGeomField     "CreateGeomField"
#define OLCCurveGeometries     "CurveGeometries"
#define OLCMeasuredGeometries  "MeasuredGeometries"
#define OLCZGeometries         "ZGeometries"
#define OLCRename              "Rename"
#define OLCFastGetArrowStream  "FastGetArrowStream"
#define OLCFastWriteArrowBatch "FastWriteArrowBatch"

#define ODsCCreateLayer        "CreateLayer"
#define ODsCDeleteLayer        "DeleteLayer"
#define ODsCCreateGeomFieldAfterCreateLayer   "CreateGeomFieldAfterCreateLayer"
#define ODsCCurveGeometries    "CurveGeometries"
#define ODsCTransactions       "Transactions"
#define ODsCEmulatedTransactions "EmulatedTransactions"
#define ODsCMeasuredGeometries  "MeasuredGeometries"
#define ODsCZGeometries        "ZGeometries"
#define ODsCRandomLayerRead    "RandomLayerRead"
/* Note the unfortunate trailing space at the end of the string */
#define ODsCRandomLayerWrite   "RandomLayerWrite "

#define ODrCCreateDataSource   "CreateDataSource"
#define ODrCDeleteDataSource   "DeleteDataSource"

#define OLMD_FID64             "OLMD_FID64"

#define GEOS_PREC_NO_TOPO          1
#define GEOS_PREC_KEEP_COLLAPSED   2

#endif

#if defined(SWIGCSHARP) || defined(SWIGJAVA) || defined(SWIGPYTHON)

#define OGRERR_NONE                0
#define OGRERR_NOT_ENOUGH_DATA     1    /* not enough data to deserialize */
#define OGRERR_NOT_ENOUGH_MEMORY   2
#define OGRERR_UNSUPPORTED_GEOMETRY_TYPE 3
#define OGRERR_UNSUPPORTED_OPERATION 4
#define OGRERR_CORRUPT_DATA        5
#define OGRERR_FAILURE             6
#define OGRERR_UNSUPPORTED_SRS     7
#define OGRERR_INVALID_HANDLE      8
#define OGRERR_NON_EXISTING_FEATURE 9

#endif

#if defined(SWIGPYTHON)
%include ogr_python.i
#elif defined(SWIGCSHARP)
%include ogr_csharp.i
#elif defined(SWIGJAVA)
%include ogr_java.i
#else
%include gdal_typemaps.i
#endif

/*
 * We need to import osr.i here so the ogr module knows about the
 * wrapper for SpatialReference and CoordinateSystem from osr.
 * These types are used in Geometry::Transform() among others.
 */
#define FROM_OGR_I
%import osr.i

#ifndef FROM_GDAL_I
/* For Python we don't import, but include MajorObject.i to avoid */
/* cyclic dependency between gdal.py and ogr.py. */
/* We should probably define a new module for MajorObject, or merge gdal and ogr */
/* modules */
#if defined(SWIGPYTHON)
%{
#include "gdal.h"
%}
typedef int CPLErr;
#define FROM_PYTHON_OGR_I
%include MajorObject.i
#undef FROM_PYTHON_OGR_I
#else /* defined(SWIGPYTHON) */
%import MajorObject.i
%import Dataset_import.i
#endif /* defined(SWIGPYTHON) */
#endif /* FROM_GDAL_I */

/************************************************************************/
/*                               OGRGetGEOSVersion                      */
/************************************************************************/
%inline %{
int GetGEOSVersionMajor() {
    int num;
    OGRGetGEOSVersion(&num, NULL, NULL);
    return num;
}

int GetGEOSVersionMinor() {
    int num;
    OGRGetGEOSVersion(NULL, &num, NULL);
    return num;
}

int GetGEOSVersionMicro() {
    int num;
    OGRGetGEOSVersion(NULL, NULL, &num);
    return num;
}
%}

/************************************************************************/
/*                               OGREnvelope                            */
/************************************************************************/

#if defined(SWIGCSHARP)
%rename (Envelope) OGREnvelope;
typedef struct
{
    double      MinX;
    double      MaxX;
    double      MinY;
    double      MaxY;
} OGREnvelope;

%rename (Envelope3D) OGREnvelope3D;
typedef struct
{
    double      MinX;
    double      MaxX;
    double      MinY;
    double      MaxY;
    double      MinZ;
    double      MaxZ;
} OGREnvelope3D;
#endif

/************************************************************************/
/*                          OGRStyleTable                               */
/************************************************************************/

%rename (StyleTable) OGRStyleTableShadow;
class OGRStyleTableShadow {
public:

%extend {

   OGRStyleTableShadow() {
        return (OGRStyleTableShadow*) OGR_STBL_Create();
   }

   ~OGRStyleTableShadow() {
        OGR_STBL_Destroy( (OGRStyleTableH) self );
   }

   int AddStyle( const char *pszName, const char *pszStyleString )
   {
        return OGR_STBL_AddStyle( (OGRStyleTableH) self, pszName, pszStyleString);
   }

   int LoadStyleTable( const char *utf8_path )
   {
        return OGR_STBL_LoadStyleTable( (OGRStyleTableH) self, utf8_path );
   }

   int SaveStyleTable( const char *utf8_path )
   {
        return OGR_STBL_SaveStyleTable( (OGRStyleTableH) self, utf8_path );
   }

   const char* Find( const char* pszName )
   {
        return OGR_STBL_Find( (OGRStyleTableH) self, pszName );
   }

   void ResetStyleStringReading()
   {
        OGR_STBL_ResetStyleStringReading( (OGRStyleTableH) self );
   }

   const char *GetNextStyle( )
   {
        return OGR_STBL_GetNextStyle( (OGRStyleTableH) self );
   }

   const char *GetLastStyleName( )
   {
        return OGR_STBL_GetLastStyleName( (OGRStyleTableH) self );
   }
}
};

/************************************************************************/
/*                              OGRDriver                               */
/************************************************************************/

#ifndef FROM_GDAL_I

#ifdef SWIGPYTHON
/* In Python, gdal.Driver and ogr.Driver are equivalent */
typedef GDALDriverShadow OGRDriverShadow;
#else

%rename (Driver) OGRDriverShadow;

#ifdef SWIGCSHARP
/* Because of issue with CSharp to handle different inheritance from class in different namespaces  */
class OGRDriverShadow {
#else
class OGRDriverShadow : public GDALMajorObjectShadow {
#endif
  OGRDriverShadow();
  ~OGRDriverShadow();
public:
%extend {

%immutable;
  char const *name;
%mutable;

%newobject CreateDataSource;
#ifdef SWIGPYTHON
%thread;
#endif
#ifndef SWIGJAVA
%feature( "kwargs" ) CreateDataSource;
#endif
  OGRDataSourceShadow *CreateDataSource( const char *utf8_path,
                                    char **options = 0 ) {
    OGRDataSourceShadow *ds = (OGRDataSourceShadow*) OGR_Dr_CreateDataSource( self, utf8_path, options);
    return ds;
  }
#ifdef SWIGPYTHON
%nothread;
#endif

%newobject CopyDataSource;
#ifdef SWIGPYTHON
%thread;
#endif
#ifndef SWIGJAVA
%feature( "kwargs" ) CopyDataSource;
#endif
%apply Pointer NONNULL {OGRDataSourceShadow *copy_ds};
  OGRDataSourceShadow *CopyDataSource( OGRDataSourceShadow* copy_ds,
                                  const char* utf8_path,
                                  char **options = 0 ) {
    OGRDataSourceShadow *ds = (OGRDataSourceShadow*) OGR_Dr_CopyDataSource(self, copy_ds, utf8_path, options);
    return ds;
  }
%clear OGRDataSourceShadow *copy_ds;
#ifdef SWIGPYTHON
%nothread;
#endif

%newobject Open;
#ifdef SWIGPYTHON
%thread;
#endif
#ifndef SWIGJAVA
%feature( "kwargs" ) Open;
#endif
  OGRDataSourceShadow *Open( const char* utf8_path,
                        int update=0 ) {
    CPLErrorReset();
    OGRDataSourceShadow* ds = (OGRDataSourceShadow*) OGR_Dr_Open(self, utf8_path, update);
#ifndef SWIGPYTHON
    if( CPLGetLastErrorType() == CE_Failure && ds != NULL )
    {
        CPLDebug(
            "SWIG",
            "OGR_Dr_Open() succeeded, but an error is posted, so we destroy"
            " the datasource and fail at swig level.\nError:%s",
            CPLGetLastErrorMsg() );
        OGRReleaseDataSource(ds);
        ds = NULL;
    }
#endif
    return ds;
  }
#ifdef SWIGPYTHON
%nothread;
#endif

#ifdef SWIGJAVA
  OGRErr DeleteDataSource( const char *utf8_path ) {
#else
  int DeleteDataSource( const char *utf8_path ) {
#endif
    return OGR_Dr_DeleteDataSource( self, utf8_path );
  }

%apply Pointer NONNULL {const char * cap};
  bool TestCapability (const char *cap) {
    return (OGR_Dr_TestCapability(self, cap) > 0);
  }

  const char * GetName() {
    return OGR_Dr_GetName( self );
  }

  /* Added in GDAL 1.8.0 */
  void Register() {
    OGRRegisterDriver( self );
  }

  /* Added in GDAL 1.8.0 */
  void Deregister() {
    OGRDeregisterDriver( self );
  }

} /* %extend */
}; /* class OGRDriverShadow */

#endif

/************************************************************************/
/*                            OGRDataSource                             */
/************************************************************************/

#ifdef SWIGPYTHON
/* In Python, ogr.DataSource and gdal.Dataset are equivalent */
typedef GDALDatasetShadow OGRDataSourceShadow;

#else

%rename (DataSource) OGRDataSourceShadow;

#ifdef SWIGCSHARP
/* Because of issue with CSharp to handle different inheritance from class in different namespaces  */
class OGRDataSourceShadow {
#else
class OGRDataSourceShadow : public GDALMajorObjectShadow {
#endif
  OGRDataSourceShadow() {
  }
public:
%extend {

%immutable;
  char const *name;
%mutable;


  ~OGRDataSourceShadow() {
    OGRReleaseDataSource(self);
  }

  CPLErr Close() {
    return GDALClose(self);
  }

  int GetRefCount() {
    return OGR_DS_GetRefCount(self);
  }

  int GetSummaryRefCount() {
    return OGR_DS_GetSummaryRefCount(self);
  }

  int GetLayerCount() {
    return OGR_DS_GetLayerCount(self);
  }

  OGRDriverShadow * GetDriver() {
    return (OGRDriverShadow *) OGR_DS_GetDriver( self );
  }

  const char * GetName() {
    return OGR_DS_GetName(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetNameAsByteArray() {
    return OGR_DS_GetName(self);
  }
#endif

  OGRErr DeleteLayer(int index){
    return OGR_DS_DeleteLayer(self, index);
  }

  OGRErr SyncToDisk() {
    return OGR_DS_SyncToDisk(self);
  }

  void FlushCache() {
    GDALFlushCache( self );
  }

  /* Note that datasources own their layers */
#ifndef SWIGJAVA
  %feature( "kwargs" ) CreateLayer;
#endif
  OGRLayerShadow *CreateLayer(const char* name,
              OSRSpatialReferenceShadow* srs=NULL,
              OGRwkbGeometryType geom_type=wkbUnknown,
              char** options=0) {
    OGRLayerShadow* layer = (OGRLayerShadow*) OGR_DS_CreateLayer( self,
								  name,
								  srs,
								  geom_type,
								  options);
    return layer;
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) CopyLayer;
#endif
%apply Pointer NONNULL {OGRLayerShadow *src_layer};
  OGRLayerShadow *CopyLayer(OGRLayerShadow *src_layer,
            const char* new_name,
            char** options=0) {
    OGRLayerShadow* layer = (OGRLayerShadow*) OGR_DS_CopyLayer( self,
                                                      src_layer,
                                                      new_name,
                                                      options);
    return layer;
  }

#ifdef SWIGJAVA
  OGRLayerShadow *GetLayerByIndex( int index ) {
#else
  OGRLayerShadow *GetLayerByIndex( int index=0) {
#endif
    OGRLayerShadow* layer = (OGRLayerShadow*) OGR_DS_GetLayer(self, index);
    return layer;
  }

  OGRLayerShadow *GetLayerByName( const char* layer_name) {
    OGRLayerShadow* layer = (OGRLayerShadow*) OGR_DS_GetLayerByName(self, layer_name);
    return layer;
  }

  bool TestCapability(const char * cap) {
    return (OGR_DS_TestCapability(self, cap) > 0);
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) ExecuteSQL;
#endif
  %apply Pointer NONNULL {const char * statement};
  OGRLayerShadow *ExecuteSQL(const char* statement,
                        OGRGeometryShadow* spatialFilter=NULL,
                        const char* dialect="") {
    OGRLayerShadow* layer = (OGRLayerShadow*) OGR_DS_ExecuteSQL((OGRDataSourceShadow*)self,
                                                      statement,
                                                      spatialFilter,
                                                      dialect);
    return layer;
  }

  OGRErr AbortSQL(){
    return GDALDatasetAbortSQL((OGRDataSourceShadow*)self);
  }

%apply SWIGTYPE *DISOWN {OGRLayerShadow *layer};
  void ReleaseResultSet(OGRLayerShadow *layer){
    OGR_DS_ReleaseResultSet(self, layer);
  }
%clear OGRLayerShadow *layer;

  OGRStyleTableShadow *GetStyleTable() {
    return (OGRStyleTableShadow*) OGR_DS_GetStyleTable(self);
  }

  void SetStyleTable(OGRStyleTableShadow* table) {
    if( table != NULL )
        OGR_DS_SetStyleTable(self, (OGRStyleTableH) table);
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) StartTransaction;
#endif
  OGRErr StartTransaction(int force = FALSE)
  {
    return GDALDatasetStartTransaction(self, force);
  }

  OGRErr CommitTransaction()
  {
    return GDALDatasetCommitTransaction(self);
  }

  OGRErr RollbackTransaction()
  {
    return GDALDatasetRollbackTransaction(self);
  }
} /* %extend */


}; /* class OGRDataSourceShadow */

#endif /* not SWIGPYTHON */

#endif /* FROM_GDAL_I */

#ifdef SWIGPYTHON

class ArrowArray {
  ArrowArray();
public:
%extend {

  ArrowArray() {
    return (struct ArrowArray* )calloc(1, sizeof(struct ArrowArray));
  }

  ~ArrowArray() {
    if( self->release )
      self->release(self);
    free(self);
  }

  VoidPtrAsLong _getPtr() {
    return self;
  }

  GIntBig GetChildrenCount() {
    return self->n_children;
  }

  GIntBig GetLength() {
    return self->length;
  }

} /* %extend */

}; /* class ArrowArray */

class ArrowSchema {
  ArrowSchema();
public:
%extend {

  ArrowSchema() {
    return (struct ArrowSchema* )calloc(1, sizeof(struct ArrowSchema));
  }

  ~ArrowSchema() {
    if( self->release )
      self->release(self);
    free(self);
  }

  VoidPtrAsLong _getPtr() {
    return self;
  }

  const char* GetName() {
    return self->name;
  }

  GIntBig GetChildrenCount() {
    return self->n_children;
  }

  const ArrowSchema* GetChild(int iChild) {
    if( iChild < 0 || iChild >= self->n_children )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong index");
        return NULL;
    }
    return self->children[iChild];
  }

} /* %extend */

}; /* class ArrowSchema */

class ArrowArrayStream {
  ArrowArrayStream();
public:
%extend {

  ~ArrowArrayStream() {
    if( self->release )
      self->release(self);
    free(self);
  }

%newobject GetSchema;
  ArrowSchema* GetSchema()
  {
      struct ArrowSchema* schema = (struct ArrowSchema* )malloc(sizeof(struct ArrowSchema));
      if( self->get_schema(self, schema) == 0 )
      {
          return schema;
      }
      else
      {
          free(schema);
          return NULL;
      }
  }

%newobject GetNextRecordBatch;
  ArrowArray* GetNextRecordBatch(char** options = NULL)
  {
      struct ArrowArray* array = (struct ArrowArray* )malloc(sizeof(struct ArrowArray));
      if( self->get_next(self, array) == 0 && array->release != NULL )
      {
          return array;
      }
      else
      {
          free(array);
          return NULL;
      }
  }
} /* %extend */


}; /* class ArrowArrayStream */
#endif

#ifdef SWIGPYTHON
// Implements __arrow_c_stream__ export interface:
// https://arrow.apache.org/docs/format/CDataInterface/PyCapsuleInterface.html#create-a-pycapsule
%{
static void ReleaseArrowArrayStreamPyCapsule(PyObject* capsule) {
    struct ArrowArrayStream* stream =
        (struct ArrowArrayStream*)PyCapsule_GetPointer(capsule, "arrow_array_stream");
    if (stream->release != NULL) {
        stream->release(stream);
    }
    CPLFree(stream);
}

static char** ParseArrowMetadata(const char *pabyMetadata)
{
    char** ret = NULL;
    int32_t nKVP;
    memcpy(&nKVP, pabyMetadata, sizeof(int32_t));
    pabyMetadata += sizeof(int32_t);
    for (int i = 0; i < nKVP; ++i)
    {
        int32_t nSizeKey;
        memcpy(&nSizeKey, pabyMetadata, sizeof(int32_t));
        pabyMetadata += sizeof(int32_t);
        std::string osKey;
        osKey.assign(pabyMetadata, nSizeKey);
        pabyMetadata += nSizeKey;

        int32_t nSizeValue;
        memcpy(&nSizeValue, pabyMetadata, sizeof(int32_t));
        pabyMetadata += sizeof(int32_t);
        std::string osValue;
        osValue.assign(pabyMetadata, nSizeValue);
        pabyMetadata += nSizeValue;

        ret = CSLSetNameValue(ret, osKey.c_str(), osValue.c_str());
    }

    return ret;
}

// Create output fields using CreateFieldFromArrowSchema()
static bool CreateFieldsFromArrowSchema(OGRLayerH hDstLayer,
                                        const struct ArrowSchema* schemaSrc,
                                        char** options)
{
    for (int i = 0; i < schemaSrc->n_children; ++i)
    {
        const char *metadata =
            schemaSrc->children[i]->metadata;
        if( metadata )
        {
            char** keyValues = ParseArrowMetadata(metadata);
            const char *ARROW_EXTENSION_NAME_KEY = "ARROW:extension:name";
            const char *EXTENSION_NAME_OGC_WKB = "ogc.wkb";
            const char *EXTENSION_NAME_GEOARROW_WKB = "geoarrow.wkb";
            const char* value = CSLFetchNameValue(keyValues, ARROW_EXTENSION_NAME_KEY);
            const bool bSkip = ( value && (EQUAL(value, EXTENSION_NAME_OGC_WKB) || EQUAL(value, EXTENSION_NAME_GEOARROW_WKB)) );
            CSLDestroy(keyValues);
            if( bSkip )
                continue;
        }

        const char *pszFieldName =
            schemaSrc->children[i]->name;
        if (!EQUAL(pszFieldName, "OGC_FID") &&
            !EQUAL(pszFieldName, "wkb_geometry") &&
            !OGR_L_CreateFieldFromArrowSchema(
                hDstLayer, schemaSrc->children[i], options))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot create field %s",
                     pszFieldName);
            return false;
        }
    }
    return true;
}

%}

#endif

/************************************************************************/
/*                               OGRLayer                               */
/************************************************************************/

%rename (Layer) OGRLayerShadow;
#ifdef SWIGCSHARP
/* Because of issue with CSharp to handle different inheritance from class in different namespaces  */
class OGRLayerShadow {
#else
class OGRLayerShadow : public GDALMajorObjectShadow {
#endif
  OGRLayerShadow();
  ~OGRLayerShadow();
public:
%extend {

#ifndef SWIGCSHARP
  GDALDatasetShadow* GetDataset()
  {
      return OGR_L_GetDataset(self);
  }
#endif

  %apply Pointer NONNULL {const char * new_name};
  OGRErr Rename(const char* new_name) {
    return OGR_L_Rename( self, new_name);
  }
  %clear const char* new_name;

  int GetRefCount() {
    return OGR_L_GetRefCount(self);
  }

  void SetSpatialFilter(OGRGeometryShadow* filter) {
    OGR_L_SetSpatialFilter (self, filter);
  }

  void SetSpatialFilterRect( double minx, double miny,
                             double maxx, double maxy) {
    OGR_L_SetSpatialFilterRect(self, minx, miny, maxx, maxy);
  }

  void SetSpatialFilter(int iGeomField, OGRGeometryShadow* filter) {
    OGR_L_SetSpatialFilterEx (self, iGeomField, filter);
  }

  void SetSpatialFilterRect( int iGeomField, double minx, double miny,
                             double maxx, double maxy) {
    OGR_L_SetSpatialFilterRectEx(self, iGeomField, minx, miny, maxx, maxy);
  }

  OGRGeometryShadow *GetSpatialFilter() {
    return (OGRGeometryShadow *) OGR_L_GetSpatialFilter(self);
  }

#ifdef SWIGCSHARP
  %apply ( const char *utf8_path ) { (char* filter_string) };
#endif
  OGRErr SetAttributeFilter(char* filter_string) {
    return OGR_L_SetAttributeFilter((OGRLayerShadow*)self, filter_string);
  }
#ifdef SWIGCSHARP
  %clear (char* filter_string);
#endif

  void ResetReading() {
    OGR_L_ResetReading(self);
  }

  const char * GetName() {
    return OGR_L_GetName(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetNameAsByteArray() {
    return OGR_L_GetName(self);
  }
#endif

  /* Added in OGR 1.8.0 */
  OGRwkbGeometryType GetGeomType() {
    return (OGRwkbGeometryType) OGR_L_GetGeomType(self);
  }

  const char * GetGeometryColumn() {
    return OGR_L_GetGeometryColumn(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetGeometryColumnAsByteArray() {
    return OGR_L_GetGeometryColumn(self);
  }
#endif

  const char * GetFIDColumn() {
    return OGR_L_GetFIDColumn(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetFIDColumnAsByteArray() {
    return OGR_L_GetFIDColumn(self);
  }
#endif

%newobject GetFeature;
  OGRFeatureShadow *GetFeature(GIntBig fid) {
    return (OGRFeatureShadow*) OGR_L_GetFeature(self, fid);
  }

%newobject GetNextFeature;
  OGRFeatureShadow *GetNextFeature() {
    return (OGRFeatureShadow*) OGR_L_GetNextFeature(self);
  }

  OGRErr SetNextByIndex(GIntBig new_index) {
    return OGR_L_SetNextByIndex(self, new_index);
  }

%apply Pointer NONNULL {OGRFeatureShadow *feature};
  OGRErr SetFeature(OGRFeatureShadow *feature) {
    return OGR_L_SetFeature(self, feature);
  }

  OGRErr CreateFeature(OGRFeatureShadow *feature) {
    return OGR_L_CreateFeature(self, feature);
  }

  OGRErr UpsertFeature(OGRFeatureShadow *feature) {
    return OGR_L_UpsertFeature(self, feature);
  }

#if defined(SWIGCSHARP)
%apply int PINNED[] {int *panUpdatedFieldsIdx};
%apply int PINNED[] {int *panUpdatedGeomFieldsIdx};
#else
%apply (int nList, int *pList ) { (int nUpdatedFieldsCount, int *panUpdatedFieldsIdx ) };
%apply (int nList, int *pList ) { (int nUpdatedGeomFieldsCount, int *panUpdatedGeomFieldsIdx ) };
#endif
  OGRErr UpdateFeature(OGRFeatureShadow *feature,
                       int nUpdatedFieldsCount,
                       const int *panUpdatedFieldsIdx,
                       int nUpdatedGeomFieldsCount,
                       const int *panUpdatedGeomFieldsIdx,
                       bool bUpdateStyleString) {
    return OGR_L_UpdateFeature(self, feature,
                               nUpdatedFieldsCount,
                               panUpdatedFieldsIdx,
                               nUpdatedGeomFieldsCount,
                               panUpdatedGeomFieldsIdx,
                               bUpdateStyleString);
  }
#if defined(SWIGCSHARP)
%clear int *panUpdatedFieldsIdx;
%clear int *panUpdatedGeomFieldsIdx;
#else
%clear (int nUpdatedFieldsCount, int *panUpdatedFieldsIdx );
%clear (int nUpdatedGeomFieldsCount, int *panUpdatedGeomFieldsIdx );
#endif
%clear OGRFeatureShadow *feature;

  OGRErr DeleteFeature(GIntBig fid) {
    return OGR_L_DeleteFeature(self, fid);
  }

  OGRErr SyncToDisk() {
    return OGR_L_SyncToDisk(self);
  }

  %newobject GetLayerDefn;
  OGRFeatureDefnShadow *GetLayerDefn() {
    auto defn = (OGRFeatureDefnShadow*) OGR_L_GetLayerDefn(self);
    if (defn)
        OGR_FD_Reference(defn);
    return defn;
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) GetFeatureCount;
#endif
  GIntBig GetFeatureCount(int force=1) {
    return OGR_L_GetFeatureCount(self, force);
  }

#if defined(SWIGCSHARP)
  %feature( "kwargs" ) GetExtent;
  OGRErr GetExtent(OGREnvelope* extent, int force=1) {
    return OGR_L_GetExtent(self, extent, force);
  }
#elif defined(SWIGPYTHON)
  %feature( "kwargs" ) GetExtent;
  void GetExtent(double argout[4], int* isvalid = NULL, int force = 1, int can_return_null = 0, int geom_field = 0 ) {
    OGRErr eErr = OGR_L_GetExtentEx(self, geom_field, (OGREnvelope*)argout, force);
    if (can_return_null)
        *isvalid = (eErr == OGRERR_NONE);
    else
        *isvalid = TRUE;
    return;
  }
  %feature( "kwargs" ) GetExtent3D;
  void GetExtent3D(double argout[6], int* isvalid = NULL, int force = 1, int can_return_null = 0, int geom_field = 0 ) {
    OGRErr eErr = OGR_L_GetExtent3D(self, geom_field, (OGREnvelope3D*)argout, force);
    if (can_return_null)
        *isvalid = (eErr == OGRERR_NONE);
    else
        *isvalid = TRUE;
    return;
  }
#else
#ifndef SWIGJAVA
  %feature( "kwargs" ) GetExtent;
  OGRErr GetExtent(double argout[4], int force = 1) {
#else
  OGRErr GetExtent(double argout[4], int force) {
#endif
    return OGR_L_GetExtent(self, (OGREnvelope*)argout, force);
  }
#endif

  bool TestCapability(const char* cap) {
    return (OGR_L_TestCapability(self, cap) > 0);
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) CreateField;
#endif
%apply Pointer NONNULL {OGRFieldDefnShadow *field_def};
  OGRErr CreateField(OGRFieldDefnShadow* field_def, int approx_ok = 1) {
    return OGR_L_CreateField(self, field_def, approx_ok);
  }
%clear OGRFieldDefnShadow *field_def;

  OGRErr DeleteField(int iField)
  {
    return OGR_L_DeleteField(self, iField);
  }

  OGRErr ReorderField(int iOldFieldPos, int iNewFieldPos)
  {
    return OGR_L_ReorderField(self, iOldFieldPos, iNewFieldPos);
  }

  OGRErr ReorderFields(int nList, int *pList)
  {
    if (nList != OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(self)))
    {
      CPLError(CE_Failure, CPLE_IllegalArg,
               "List should have %d elements",
               OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(self)));
      return OGRERR_FAILURE;
    }
    return OGR_L_ReorderFields(self, pList);
  }

%apply Pointer NONNULL {OGRFieldDefnShadow *field_def};
  OGRErr AlterFieldDefn(int iField, OGRFieldDefnShadow* field_def, int nFlags)
  {
    return OGR_L_AlterFieldDefn(self, iField, field_def, nFlags);
  }
%clear OGRFieldDefnShadow *field_def;

%apply Pointer NONNULL {OGRGeomFieldDefnShadow *field_def};
  OGRErr AlterGeomFieldDefn(int iGeomField, const OGRGeomFieldDefnShadow* field_def, int nFlags)
  {
    return OGR_L_AlterGeomFieldDefn(self, iGeomField, const_cast<OGRGeomFieldDefnShadow*>(field_def), nFlags);
  }
%clear OGRGeomFieldDefnShadow *field_def;

#ifndef SWIGJAVA
  %feature( "kwargs" ) CreateGeomField;
#endif
%apply Pointer NONNULL {OGRGeomFieldDefnShadow *field_def};
  OGRErr CreateGeomField(OGRGeomFieldDefnShadow* field_def, int approx_ok = 1) {
    return OGR_L_CreateGeomField(self, field_def, approx_ok);
  }
%clear OGRGeomFieldDefnShadow *field_def;

  OGRErr StartTransaction() {
    return OGR_L_StartTransaction(self);
  }

  OGRErr CommitTransaction() {
    return OGR_L_CommitTransaction(self);
  }

  OGRErr RollbackTransaction() {
    return OGR_L_RollbackTransaction(self);
  }

  int FindFieldIndex( const char *pszFieldName, int bExactMatch ) {
    return OGR_L_FindFieldIndex(self, pszFieldName, bExactMatch );
  }

  %newobject GetSpatialRef;
  OSRSpatialReferenceShadow *GetSpatialRef() {
    OGRSpatialReferenceH ref =  OGR_L_GetSpatialRef(self);
    if( ref )
        OSRReference(ref);
    return (OSRSpatialReferenceShadow*) ref;
  }

  GIntBig GetFeaturesRead() {
    return OGR_L_GetFeaturesRead(self);
  }

  OGRErr SetIgnoredFields( const char **options ) {
    return OGR_L_SetIgnoredFields( self, options );
  }

%apply Pointer NONNULL {OGRFieldDefnShadow *method_layer};
%apply Pointer NONNULL {OGRFieldDefnShadow *result_layer};

#ifndef SWIGJAVA
  %feature( "kwargs" ) Intersection;
#endif
  OGRErr Intersection( OGRLayerShadow *method_layer,
                       OGRLayerShadow *result_layer,
                       char **options=NULL,
                       GDALProgressFunc callback=NULL,
                       void* callback_data=NULL ) {
    return OGR_L_Intersection( self, method_layer, result_layer, options, callback, callback_data );
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) Union;
#endif
  OGRErr Union( OGRLayerShadow *method_layer,
                OGRLayerShadow *result_layer,
                char **options=NULL,
                GDALProgressFunc callback=NULL,
                void* callback_data=NULL ) {
    return OGR_L_Union( self, method_layer, result_layer, options, callback, callback_data );
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) SymDifference;
#endif
  OGRErr SymDifference( OGRLayerShadow *method_layer,
                        OGRLayerShadow *result_layer,
                        char **options=NULL,
                        GDALProgressFunc callback=NULL,
                        void* callback_data=NULL ) {
    return OGR_L_SymDifference( self, method_layer, result_layer, options, callback, callback_data );
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) Identity;
#endif
  OGRErr Identity( OGRLayerShadow *method_layer,
                   OGRLayerShadow *result_layer,
                   char **options=NULL,
                   GDALProgressFunc callback=NULL,
                   void* callback_data=NULL ) {
    return OGR_L_Identity( self, method_layer, result_layer, options, callback, callback_data );
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) Update;
#endif
  OGRErr Update( OGRLayerShadow *method_layer,
                 OGRLayerShadow *result_layer,
                 char **options=NULL,
                 GDALProgressFunc callback=NULL,
                 void* callback_data=NULL ) {
    return OGR_L_Update( self, method_layer, result_layer, options, callback, callback_data );
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) Clip;
#endif
  OGRErr Clip( OGRLayerShadow *method_layer,
               OGRLayerShadow *result_layer,
               char **options=NULL,
               GDALProgressFunc callback=NULL,
               void* callback_data=NULL ) {
    return OGR_L_Clip( self, method_layer, result_layer, options, callback, callback_data );
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) Erase;
#endif
  OGRErr Erase( OGRLayerShadow *method_layer,
                OGRLayerShadow *result_layer,
                char **options=NULL,
                GDALProgressFunc callback=NULL,
                void* callback_data=NULL ) {
    return OGR_L_Erase( self, method_layer, result_layer, options, callback, callback_data );
  }

  OGRStyleTableShadow *GetStyleTable() {
    return (OGRStyleTableShadow*) OGR_L_GetStyleTable(self);
  }

  void SetStyleTable(OGRStyleTableShadow* table) {
    if( table != NULL )
        OGR_L_SetStyleTable(self, (OGRStyleTableH) table);
  }

#ifdef SWIGPYTHON

    PyObject* ExportArrowArrayStreamPyCapsule(char** options = NULL)
    {
        struct ArrowArrayStream* stream =
            (struct ArrowArrayStream*)CPLMalloc(sizeof(struct ArrowArrayStream));

        const int success = OGR_L_GetArrowStream(self, stream, options);

        PyObject* ret;
        SWIG_PYTHON_THREAD_BEGIN_BLOCK;
        if( success )
        {
            ret = PyCapsule_New(stream, "arrow_array_stream", ReleaseArrowArrayStreamPyCapsule);
        }
        else
        {
            CPLFree(stream);
            Py_INCREF(Py_None);
            ret = Py_None;
        }

        SWIG_PYTHON_THREAD_END_BLOCK;

        return ret;
    }

%newobject GetArrowStream;
  ArrowArrayStream* GetArrowStream(char** options = NULL) {
      struct ArrowArrayStream* stream = (struct ArrowArrayStream* )malloc(sizeof(struct ArrowArrayStream));
      if( OGR_L_GetArrowStream(self, stream, options) )
          return stream;
      else
      {
          free(stream);
          return NULL;
      }
  }
#endif

#ifdef SWIGPYTHON
    void IsArrowSchemaSupported(const struct ArrowSchema *schema, bool* pbRet, char **errorMsg, char** options = NULL)
    {
        *pbRet = OGR_L_IsArrowSchemaSupported(self, schema, options, errorMsg);
    }
#endif

#ifdef SWIGPYTHON
    OGRErr CreateFieldFromArrowSchema(const struct ArrowSchema *schema, char** options = NULL)
    {
        return OGR_L_CreateFieldFromArrowSchema(self, schema, options) ? OGRERR_NONE : OGRERR_FAILURE;
    }
#endif

#ifdef SWIGPYTHON
    OGRErr WriteArrowBatch(const struct ArrowSchema *schema, struct ArrowArray *array, char** options = NULL)
    {
        return OGR_L_WriteArrowBatch(self, schema, array, options) ? OGRERR_NONE : OGRERR_FAILURE;
    }

    OGRErr WriteArrowStreamCapsule(PyObject* capsule, int createFieldsFromSchema, char** options = NULL)
    {
        ArrowArrayStream* stream = (ArrowArrayStream*)PyCapsule_GetPointer(capsule, "arrow_array_stream");
        if( !stream )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "PyCapsule_GetPointer(capsule, \"arrow_array_stream\") failed");
            return OGRERR_FAILURE;
        }
        if( stream->release == NULL )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "stream->release == NULL");
            return OGRERR_FAILURE;
        }

        ArrowSchema schema;
        if( stream->get_schema(stream, &schema) != 0 )
        {
            stream->release(stream);
            return OGRERR_FAILURE;
        }

        if( createFieldsFromSchema == TRUE ||
            (createFieldsFromSchema == -1 && OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(self)) == 0) )
        {
            if( !CreateFieldsFromArrowSchema(self, &schema, options) )
            {
                schema.release(&schema);
                stream->release(stream);
                return OGRERR_FAILURE;
            }
        }

        while( true )
        {
            ArrowArray array;
            if( stream->get_next(stream, &array) == 0 )
            {
                if( array.release == NULL )
                    break;
                if( !OGR_L_WriteArrowBatch(self, &schema, &array, options) )
                {
                    if( array.release )
                        array.release(&array);
                    schema.release(&schema);
                    stream->release(stream);
                    return OGRERR_FAILURE;
                }
                if( array.release )
                    array.release(&array);
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined, "stream->get_next(stream, &array) failed");
                schema.release(&schema);
                stream->release(stream);
                return OGRERR_FAILURE;
            }
        }
        schema.release(&schema);
        stream->release(stream);
        return OGRERR_NONE;
    }

    OGRErr WriteArrowSchemaAndArrowArrayCapsule(PyObject* schemaCapsule, PyObject* arrayCapsule, int createFieldsFromSchema, char** options = NULL)
    {
        ArrowSchema* schema = (ArrowSchema*)PyCapsule_GetPointer(schemaCapsule, "arrow_schema");
        if( !schema )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "PyCapsule_GetPointer(schemaCapsule, \"arrow_schema\") failed");
            return OGRERR_FAILURE;
        }
        if( schema->release == NULL )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "schema->release == NULL");
            return OGRERR_FAILURE;
        }

        if( createFieldsFromSchema == TRUE ||
            (createFieldsFromSchema == -1 && OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(self)) == 0) )
        {
            if( !CreateFieldsFromArrowSchema(self, schema, options) )
            {
                schema->release(schema);
                return OGRERR_FAILURE;
            }
        }

        ArrowArray* array = (ArrowArray*)PyCapsule_GetPointer(arrayCapsule, "arrow_array");
        if( !array )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "PyCapsule_GetPointer(arrayCapsule, \"arrow_array\") failed");
            schema->release(schema);
            return OGRERR_FAILURE;
        }
        if( array->release == NULL )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "array->release == NULL");
            schema->release(schema);
            return OGRERR_FAILURE;
        }

        OGRErr eErr = OGRERR_NONE;
        if( !OGR_L_WriteArrowBatch(self, schema, array, options) )
        {
            eErr = OGRERR_FAILURE;
        }

        if( schema->release )
            schema->release(schema);
        if( array->release )
            array->release(array);
        return eErr;
    }
#endif

#ifdef SWIGPYTHON
    %feature( "kwargs" ) GetGeometryTypes;
    void GetGeometryTypes(OGRGeometryTypeCounter** ppRet, int* pnEntryCount,
                          int geom_field = 0, int flags = 0,
                          GDALProgressFunc callback=NULL,
                          void* callback_data=NULL)
    {
        *ppRet = OGR_L_GetGeometryTypes(self, geom_field, flags, pnEntryCount, callback, callback_data);
    }
#endif

#ifdef SWIGPYTHON
    %feature( "kwargs" ) GetSupportedSRSList;
    void GetSupportedSRSList(OGRSpatialReferenceH** ppRet, int* pnEntryCount,
                             int geom_field = 0)
    {
        *ppRet = OGR_L_GetSupportedSRSList(self, geom_field, pnEntryCount);
    }
#endif

    OGRErr SetActiveSRS(int geom_field, OSRSpatialReferenceShadow* srs)
    {
        return OGR_L_SetActiveSRS(self, geom_field, srs);
    }

} /* %extend */


}; /* class OGRLayerShadow */

/************************************************************************/
/*                              OGRFeature                              */
/************************************************************************/

#ifdef SWIGJAVA
%{
typedef int* retIntArray;
typedef double* retDoubleArray;
%}
#endif

#ifdef SWIGPYTHON
/* Applies perhaps to other bindings */
%apply ( const char *utf8_path ) { (const char* field_name) };
#endif

%rename (Feature) OGRFeatureShadow;
class OGRFeatureShadow {
  OGRFeatureShadow();
public:
%extend {

  ~OGRFeatureShadow() {
    OGR_F_Destroy(self);
  }

#ifndef SWIGJAVA
  %feature("kwargs") OGRFeatureShadow;
#endif
%apply Pointer NONNULL {OGRFeatureDefnShadow *feature_def};
  OGRFeatureShadow( OGRFeatureDefnShadow *feature_def ) {
      return (OGRFeatureShadow*) OGR_F_Create( feature_def );
  }

  OGRFeatureDefnShadow *GetDefnRef() {
    return (OGRFeatureDefnShadow*) OGR_F_GetDefnRef(self);
  }

  OGRErr SetGeometry(OGRGeometryShadow* geom) {
    return OGR_F_SetGeometry(self, geom);
  }

/* The feature takes over ownership of the geometry. */
/* Don't change the 'geom' name as Java bindings depends on it */
%apply SWIGTYPE *DISOWN {OGRGeometryShadow *geom};
  OGRErr SetGeometryDirectly(OGRGeometryShadow* geom) {
    return OGR_F_SetGeometryDirectly(self, geom);
  }
%clear OGRGeometryShadow *geom;

  /* Feature owns its geometry */
  OGRGeometryShadow *GetGeometryRef() {
    return (OGRGeometryShadow*) OGR_F_GetGeometryRef(self);
  }

  OGRErr SetGeomField(int iField, OGRGeometryShadow* geom) {
    return OGR_F_SetGeomField(self, iField, geom);
  }

  OGRErr SetGeomField(const char* field_name, OGRGeometryShadow* geom) {
      int iField = OGR_F_GetGeomFieldIndex(self, field_name);
      if (iField == -1)
      {
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
          return OGRERR_FAILURE;
      }
      else
          return OGR_F_SetGeomField(self, iField, geom);
  }

/* The feature takes over ownership of the geometry. */
/* Don't change the 'geom' name as Java bindings depends on it */
%apply SWIGTYPE *DISOWN {OGRGeometryShadow *geom};
  OGRErr SetGeomFieldDirectly(int iField, OGRGeometryShadow* geom) {
    return OGR_F_SetGeomFieldDirectly(self, iField, geom);
  }

  OGRErr SetGeomFieldDirectly(const char* field_name, OGRGeometryShadow* geom) {
      int iField = OGR_F_GetGeomFieldIndex(self, field_name);
      if (iField == -1)
      {
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
          return OGRERR_FAILURE;
      }
      else
          return OGR_F_SetGeomFieldDirectly(self, iField, geom);
  }
%clear OGRGeometryShadow *geom;

  /* Feature owns its geometry */
  OGRGeometryShadow *GetGeomFieldRef(int iField) {
    return (OGRGeometryShadow*) OGR_F_GetGeomFieldRef(self, iField);
  }

  /* Feature owns its geometry */
  OGRGeometryShadow *GetGeomFieldRef(const char* field_name) {
      int i = OGR_F_GetGeomFieldIndex(self, field_name);
      if (i == -1)
      {
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
          return NULL;
      }
      else
          return (OGRGeometryShadow*) OGR_F_GetGeomFieldRef(self, i);
  }

  %newobject Clone;
  OGRFeatureShadow *Clone() {
    return (OGRFeatureShadow*) OGR_F_Clone(self);
  }

%apply Pointer NONNULL {OGRFeatureShadow *feature};
  bool Equal(OGRFeatureShadow *feature) {
    return (OGR_F_Equal(self, feature) > 0);
  }
%clear OGRFeatureShadow *feature;

  int GetFieldCount() {
    return OGR_F_GetFieldCount(self);
  }

  /* ---- GetFieldDefnRef --------------------- */
  OGRFieldDefnShadow *GetFieldDefnRef(int id) {
    return (OGRFieldDefnShadow *) OGR_F_GetFieldDefnRef(self, id);
  }

  OGRFieldDefnShadow *GetFieldDefnRef(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          return (OGRFieldDefnShadow *) OGR_F_GetFieldDefnRef(self, i);
      return NULL;
  }

  /* ------------------------------------------- */

  int GetGeomFieldCount() {
    return OGR_F_GetGeomFieldCount(self);
  }

  /* ---- GetGeomFieldDefnRef --------------------- */
  OGRGeomFieldDefnShadow *GetGeomFieldDefnRef(int id) {
      return (OGRGeomFieldDefnShadow *) OGR_F_GetGeomFieldDefnRef(self, id);
  }

  OGRGeomFieldDefnShadow *GetGeomFieldDefnRef(const char* field_name) {
      int i = OGR_F_GetGeomFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          return (OGRGeomFieldDefnShadow *) OGR_F_GetGeomFieldDefnRef(self, i);
      return NULL;
  }

  /* ------------------------------------------- */

  /* ---- GetFieldAsString --------------------- */

  const char* GetFieldAsString(int id) {
    return OGR_F_GetFieldAsString(self, id);
  }

  const char* GetFieldAsString(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
      {
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
          return NULL;
      }
      else
      {
          return OGR_F_GetFieldAsString(self, i);
      }
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetFieldAsStringAsByteArray(int id) {
    return OGR_F_GetFieldAsString(self, id);
  }

  StringAsByteArray* GetFieldAsStringAsByteArray(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
      {
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
          return NULL;
      }
      else
      {
          return OGR_F_GetFieldAsString(self, i);
      }
  }
#endif

  /* ------------------------------------------- */

  /* ---- GetFieldAsISO8601DateTime ------------ */

  const char* GetFieldAsISO8601DateTime(int id, char** options = 0) {
    return OGR_F_GetFieldAsISO8601DateTime(self, id, options);
  }

  const char* GetFieldAsISO8601DateTime(const char* field_name, char** options = 0) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          return OGR_F_GetFieldAsISO8601DateTime(self, i, options);
      return NULL;
  }

  /* ------------------------------------------- */

  /* ---- GetFieldAsInteger -------------------- */

  int GetFieldAsInteger(int id) {
    return OGR_F_GetFieldAsInteger(self, id);
  }

  int GetFieldAsInteger(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
	  CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
	  return OGR_F_GetFieldAsInteger(self, i);
      return 0;
  }
  /* ------------------------------------------- */

  /* ---- GetFieldAsInteger64 ------------------ */

  GIntBig GetFieldAsInteger64(int id) {
    return OGR_F_GetFieldAsInteger64(self, id);
  }

  GIntBig GetFieldAsInteger64(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          return OGR_F_GetFieldAsInteger64(self, i);
      return 0;
  }

  /* ------------------------------------------- */

  /* ---- GetFieldAsDouble --------------------- */

  double GetFieldAsDouble(int id) {
    return OGR_F_GetFieldAsDouble(self, id);
  }

  double GetFieldAsDouble(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          return OGR_F_GetFieldAsDouble(self, i);
      return 0;
  }

  /* ------------------------------------------- */

  %apply (int *OUTPUT) {(int *)};
  %apply (float *OUTPUT) {(float *)};
  void GetFieldAsDateTime(int id, int *pnYear, int *pnMonth, int *pnDay,
			  int *pnHour, int *pnMinute, float *pfSecond,
			  int *pnTZFlag) {
      OGR_F_GetFieldAsDateTimeEx(self, id, pnYear, pnMonth, pnDay,
			       pnHour, pnMinute, pfSecond,
			       pnTZFlag);
  }

  void GetFieldAsDateTime(const char* field_name, int *pnYear, int *pnMonth, int *pnDay,
			  int *pnHour, int *pnMinute, float *pfSecond,
			  int *pnTZFlag) {
      int id = OGR_F_GetFieldIndex(self, field_name);
      if (id == -1)
	  CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
	  OGR_F_GetFieldAsDateTimeEx(self, id, pnYear, pnMonth, pnDay,
			       pnHour, pnMinute, pfSecond,
			       pnTZFlag);
  }

  %clear (int *);
  %clear (float *);

#if defined(SWIGJAVA)
  retIntArray GetFieldAsIntegerList(int id, int *nLen, const int **pList) {
      *pList = OGR_F_GetFieldAsIntegerList(self, id, nLen);
      return (retIntArray)*pList;
  }
#elif defined(SWIGCSHARP)
  %apply (int *intList) {const int *};
  %apply (int *hasval) {int *count};
  const int *GetFieldAsIntegerList(int id, int *count) {
      return OGR_F_GetFieldAsIntegerList(self, id, count);
  }
  %clear (const int *);
  %clear (int *count);
#else
  void GetFieldAsIntegerList(int id, int *nLen, const int **pList) {
      *pList = OGR_F_GetFieldAsIntegerList(self, id, nLen);
  }

  void GetFieldAsIntegerList(const char* field_name, int *nLen, const int **pList) {
      int id = OGR_F_GetFieldIndex(self, field_name);
      if (id == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          *pList = OGR_F_GetFieldAsIntegerList(self, id, nLen);
  }
#endif

#if defined(SWIGPYTHON)
  void GetFieldAsInteger64List(int id, int *nLen, const GIntBig **pList) {
      *pList = OGR_F_GetFieldAsInteger64List(self, id, nLen);
  }
#endif

#if defined(SWIGJAVA)
  retDoubleArray GetFieldAsDoubleList(int id, int *nLen, const double **pList) {
      *pList = OGR_F_GetFieldAsDoubleList(self, id, nLen);
      return (retDoubleArray)*pList;
  }
#elif defined(SWIGCSHARP)
  %apply (double *doubleList) {const double *};
  %apply (int *hasval) {int *count};
  const double *GetFieldAsDoubleList(int id, int *count) {
      return OGR_F_GetFieldAsDoubleList(self, id, count);
  }
  %clear (const double *);
  %clear (int *count);
#else
  void GetFieldAsDoubleList(int id, int *nLen, const double **pList) {
      *pList = OGR_F_GetFieldAsDoubleList(self, id, nLen);
  }

  void GetFieldAsDoubleList(const char* field_name, int *nLen, const double **pList) {
      int id = OGR_F_GetFieldIndex(self, field_name);
      if (id == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          *pList = OGR_F_GetFieldAsDoubleList(self, id, nLen);
  }
#endif

#if defined(SWIGJAVA)
  %apply (char** retAsStringArrayNoFree) {(char**)};
  char** GetFieldAsStringList(int id) {
      return OGR_F_GetFieldAsStringList(self, id);
  }
  %clear char**;
#elif defined(SWIGCSHARP) || defined(SWIGPYTHON)
  %apply (char **options) {char **};
  char **GetFieldAsStringList(int id) {
      return OGR_F_GetFieldAsStringList(self, id);
  }
  %clear (char **);
#else
  void GetFieldAsStringList(int id, char ***pList) {
      *pList = OGR_F_GetFieldAsStringList(self, id);
  }

  void GetFieldAsStringList(const char* field_name, char ***pList) {
      int id = OGR_F_GetFieldIndex(self, field_name);
      if (id == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          *pList = OGR_F_GetFieldAsStringList(self, id);
  }
#endif

#ifndef SWIGCSHARP
#ifdef SWIGJAVA
%apply (GByte* outBytes) {GByte*};
  GByte* GetFieldAsBinary(int id, int *nLen, char **pBuf) {
    GByte* pabyBlob = OGR_F_GetFieldAsBinary(self, id, nLen);
    *pBuf = (char*)VSIMalloc(*nLen);
    memcpy(*pBuf, pabyBlob, *nLen);
    return (GByte*)*pBuf;
  }

  GByte* GetFieldAsBinary(const char* field_name, int *nLen, char **pBuf) {
      int id = OGR_F_GetFieldIndex(self, field_name);
      if (id == -1)
      {
        CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
        return NULL;
      }
      else
      {
        GByte* pabyBlob = OGR_F_GetFieldAsBinary(self, id, nLen);
        *pBuf = (char*)VSIMalloc(*nLen);
        memcpy(*pBuf, pabyBlob, *nLen);
        return (GByte*)*pBuf;
      }
  }
%clear GByte*;
#else
  OGRErr GetFieldAsBinary( int id, int *nLen, char **pBuf) {
    GByte* pabyBlob = OGR_F_GetFieldAsBinary(self, id, nLen);
    *pBuf = (char*)VSIMalloc(*nLen);
    memcpy(*pBuf, pabyBlob, *nLen);
    return OGRERR_NONE;
  }

  OGRErr GetFieldAsBinary(const char* field_name, int *nLen, char **pBuf) {
      int id = OGR_F_GetFieldIndex(self, field_name);
      if (id == -1)
      {
        CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
        return OGRERR_FAILURE;
      }
      else
      {
        GByte* pabyBlob = OGR_F_GetFieldAsBinary(self, id, nLen);
        *pBuf = (char*)VSIMalloc(*nLen);
        memcpy(*pBuf, pabyBlob, *nLen);
        return OGRERR_NONE;
      }
  }
#endif /* SWIGJAVA */

#endif /* SWIGCSHARP */

  /* ---- IsFieldSet --------------------------- */
  bool IsFieldSet(int id) {
    return (OGR_F_IsFieldSet(self, id) > 0);
  }

  bool IsFieldSet(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
	  CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
	  return (OGR_F_IsFieldSet(self, i) > 0);
      return false;
  }

  /* ------------------------------------------- */

  /* ---- IsFieldNull --------------------------- */
  bool IsFieldNull(int id) {
    return (OGR_F_IsFieldNull(self, id) > 0);
  }

  bool IsFieldNull(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
	  CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
	  return (OGR_F_IsFieldNull(self, i) > 0);
      return false;
  }

  /* ------------------------------------------- */

  /* ---- IsFieldSetAndNotNull --------------------------- */
  bool IsFieldSetAndNotNull(int id) {
    return (OGR_F_IsFieldSetAndNotNull(self, id) > 0);
  }

  bool IsFieldSetAndNotNull(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
	  CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
	  return (OGR_F_IsFieldSetAndNotNull(self, i) > 0);
      return false;
  }

  /* ------------------------------------------- */

  int GetFieldIndex(const char* field_name) {
      // Do not issue an error if the field doesn't exist. It is intended to be silent
      return OGR_F_GetFieldIndex(self, field_name);
  }

  int GetGeomFieldIndex(const char* field_name) {
      // Do not issue an error if the field doesn't exist. It is intended to be silent
      return OGR_F_GetGeomFieldIndex(self, field_name);
  }

  GIntBig GetFID() {
    return OGR_F_GetFID(self);
  }

  OGRErr SetFID(GIntBig fid) {
    return OGR_F_SetFID(self, fid);
  }

  void DumpReadable() {
    OGR_F_DumpReadable(self, NULL);
  }

  retStringAndCPLFree* DumpReadableAsString(char** options=NULL) {
    return OGR_F_DumpReadableAsString(self, options);
  }

  void UnsetField(int id) {
    OGR_F_UnsetField(self, id);
  }

  void UnsetField(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          OGR_F_UnsetField(self, i);
  }

  void SetFieldNull(int id) {
    OGR_F_SetFieldNull(self, id);
  }

  void SetFieldNull(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          OGR_F_SetFieldNull(self, i);
  }

  /* ---- SetField ----------------------------- */
#ifndef SWIGCSHARP
  %apply ( tostring argin ) { (const char* value) };
#else
  %apply ( const char *utf8_path ) { (const char* value) };
#endif
  void SetField(int id, const char* value) {
    OGR_F_SetFieldString(self, id, value);
  }

  void SetField(const char* field_name, const char* value) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
          OGR_F_SetFieldString(self, i, value);
  }

  %clear (const char* value );

  void SetFieldInteger64(int id, GIntBig value) {
    OGR_F_SetFieldInteger64(self, id, value);
  }

#ifndef SWIGPYTHON
  void SetField(int id, int value) {
    OGR_F_SetFieldInteger(self, id, value);
  }

  void SetField(const char* field_name, int value) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
	  CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
	  OGR_F_SetFieldInteger(self, i, value);
  }
#endif /* SWIGPYTHON */

  void SetField(int id, double value) {
    OGR_F_SetFieldDouble(self, id, value);
  }

  void SetField(const char* field_name, double value) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
	  CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
	  OGR_F_SetFieldDouble(self, i, value);
  }

  void SetField( int id, int year, int month, int day,
                             int hour, int minute, float second,
                             int tzflag ) {
    OGR_F_SetFieldDateTimeEx(self, id, year, month, day,
                             hour, minute, second,
                             tzflag);
  }

  void SetField(const char* field_name, int year, int month, int day,
                             int hour, int minute, float second,
                             int tzflag ) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
	  CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
	  OGR_F_SetFieldDateTimeEx(self, i, year, month, day,
				 hour, minute, second,
				 tzflag);
  }

  void SetFieldIntegerList(int id, int nList, int *pList) {
      OGR_F_SetFieldIntegerList(self, id, nList, pList);
  }

#if defined(SWIGPYTHON)
  void SetFieldInteger64List(int id, int nList, GIntBig *pList) {
      OGR_F_SetFieldInteger64List(self, id, nList, pList);
  }

#endif

  void SetFieldDoubleList(int id, int nList, double *pList) {
      OGR_F_SetFieldDoubleList(self, id, nList, pList);
  }

%apply (char **options) {char **pList};
  void SetFieldStringList(int id, char **pList) {
      OGR_F_SetFieldStringList(self, id, pList);
  }
%clear char**pList;

#if defined(SWIGPYTHON)
  void _SetFieldBinary(int id, int nLen, char *pBuf) {
      OGR_F_SetFieldBinary(self, id, nLen, pBuf);
  }
#endif

  void SetFieldBinaryFromHexString(int id, const char* pszValue)
  {
     int nBytes;
     GByte* pabyBuf = CPLHexToBinary(pszValue, &nBytes );
     OGR_F_SetFieldBinary(self, id, nBytes, pabyBuf);
     CPLFree(pabyBuf);
  }

  void SetFieldBinaryFromHexString(const char* field_name, const char* pszValue)
  {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1)
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
      else
      {
          int nBytes;
          GByte* pabyBuf = CPLHexToBinary(pszValue, &nBytes );
          OGR_F_SetFieldBinary(self, i, nBytes, pabyBuf);
          CPLFree(pabyBuf);
      }
  }

  /* ------------------------------------------- */

#ifndef SWIGJAVA
  %feature("kwargs") SetFrom;
#endif
%apply Pointer NONNULL {OGRFeatureShadow *other};
  OGRErr SetFrom(OGRFeatureShadow *other, int forgiving=1) {
    return OGR_F_SetFrom(self, other, forgiving);
  }
%clear OGRFeatureShadow *other;

%apply Pointer NONNULL {OGRFeatureShadow *other};
  OGRErr SetFromWithMap(OGRFeatureShadow *other, int forgiving, int nList, int *pList) {
    if (nList != OGR_F_GetFieldCount(other))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The size of map doesn't match with the field count of the source feature");
        return OGRERR_FAILURE;
    }
    return OGR_F_SetFromWithMap(self, other, forgiving, pList);
  }
%clear OGRFeatureShadow *other;

  const char *GetStyleString() {
    return (const char*) OGR_F_GetStyleString(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray *GetStyleStringAsByteArray() {
    return OGR_F_GetStyleString(self);
  }
#endif

  void SetStyleString(const char* the_string) {
    OGR_F_SetStyleString(self, the_string);
  }

  /* ---- GetFieldType ------------------------- */
  OGRFieldType GetFieldType(int id) {
      OGRFieldDefnH fd = OGR_F_GetFieldDefnRef( self,  id );
      if (fd)
          return (OGRFieldType) OGR_Fld_GetType( fd );
      else
          return (OGRFieldType)0;
  }

  OGRFieldType GetFieldType(const char* field_name) {
      int i = OGR_F_GetFieldIndex(self, field_name);
      if (i == -1) {
          CPLError(CE_Failure, 1, FIELD_NAME_ERROR_TMPL, field_name);
          return (OGRFieldType)0;
      } else
          return (OGRFieldType) OGR_Fld_GetType( OGR_F_GetFieldDefnRef( self, i ) );
  }

  /* ------------------------------------------- */

  int Validate( int flags = OGR_F_VAL_ALL, int bEmitError = TRUE ) {
    return OGR_F_Validate(self, flags, bEmitError);
  }

  void FillUnsetWithDefault( int bNotNullableOnly = FALSE, char** options = NULL ) {
    OGR_F_FillUnsetWithDefault(self, bNotNullableOnly, options );
  }

  const char* GetNativeData () {
    return OGR_F_GetNativeData(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray *GetNativeDataAsByteArray() {
    return OGR_F_GetNativeData(self);
  }
#endif

  const char* GetNativeMediaType () {
    return OGR_F_GetNativeMediaType(self);
  }

  void SetNativeData ( const char* nativeData ) {
    OGR_F_SetNativeData(self, nativeData);
  }

  void SetNativeMediaType ( const char* nativeMediaType ) {
    OGR_F_SetNativeMediaType(self, nativeMediaType);
  }
} /* %extend */


}; /* class OGRFeatureShadow */

/************************************************************************/
/*                            OGRFeatureDefn                            */
/************************************************************************/

%rename (FeatureDefn) OGRFeatureDefnShadow;


%{
    static int ValidateOGRGeometryType(OGRwkbGeometryType field_type)
    {
        switch(field_type)
        {
            case wkbUnknown:
            case wkbPoint:
            case wkbLineString:
            case wkbPolygon:
            case wkbMultiPoint:
            case wkbMultiLineString:
            case wkbMultiPolygon:
            case wkbGeometryCollection:
            case wkbCircularString:
            case wkbCompoundCurve:
            case wkbCurvePolygon:
            case wkbMultiCurve:
            case wkbMultiSurface:
            case wkbCurve:
            case wkbSurface:
            case wkbTriangle:
            case wkbTIN:
            case wkbPolyhedralSurface:
            case wkbNone:
            /*case wkbLinearRing:*/
            case wkbCircularStringZ:
            case wkbCompoundCurveZ:
            case wkbCurvePolygonZ:
            case wkbMultiCurveZ:
            case wkbMultiSurfaceZ:
            case wkbCurveZ:
            case wkbSurfaceZ:
            case wkbTriangleZ:
            case wkbTINZ:
            case wkbPolyhedralSurfaceZ:
            case wkbPoint25D:
            case wkbLineString25D:
            case wkbPolygon25D:
            case wkbMultiPoint25D:
            case wkbMultiLineString25D:
            case wkbMultiPolygon25D:
            case wkbGeometryCollection25D:
            case wkbPointM:
            case wkbLineStringM:
            case wkbPolygonM:
            case wkbMultiPointM:
            case wkbMultiLineStringM:
            case wkbMultiPolygonM:
            case wkbGeometryCollectionM:
            case wkbCircularStringM:
            case wkbCompoundCurveM:
            case wkbCurvePolygonM:
            case wkbMultiCurveM:
            case wkbMultiSurfaceM:
            case wkbCurveM:
            case wkbSurfaceM:
            case wkbTriangleM:
            case wkbTINM:
            case wkbPolyhedralSurfaceM:
            case wkbPointZM:
            case wkbLineStringZM:
            case wkbPolygonZM:
            case wkbMultiPointZM:
            case wkbMultiLineStringZM:
            case wkbMultiPolygonZM:
            case wkbGeometryCollectionZM:
            case wkbCircularStringZM:
            case wkbCompoundCurveZM:
            case wkbCurvePolygonZM:
            case wkbMultiCurveZM:
            case wkbMultiSurfaceZM:
            case wkbCurveZM:
            case wkbSurfaceZM:
            case wkbTriangleZM:
            case wkbTINZM:
            case wkbPolyhedralSurfaceZM:
                return TRUE;
            default:
                CPLError(CE_Failure, CPLE_IllegalArg, "Illegal geometry type value");
                return FALSE;
        }
    }
%}

class OGRFeatureDefnShadow {
  OGRFeatureDefnShadow();
public:
%extend {

  ~OGRFeatureDefnShadow() {
    /*OGR_FD_Destroy(self);*/
    OGR_FD_Release( OGRFeatureDefnH(self) );
  }

#ifndef SWIGJAVA
  %feature("kwargs") OGRFeatureDefnShadow;
#endif
  OGRFeatureDefnShadow(const char* name_null_ok=NULL) {
    OGRFeatureDefnH h = OGR_FD_Create(name_null_ok);
    OGR_FD_Reference(h);
    return (OGRFeatureDefnShadow* )h;
  }

  const char* GetName(){
    return OGR_FD_GetName(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetNameAsByteArray() {
    return OGR_FD_GetName(self);
  }
#endif

  int GetFieldCount(){
    return OGR_FD_GetFieldCount(self);
  }

  /* FeatureDefns own their FieldDefns */
  OGRFieldDefnShadow* GetFieldDefn(int i){
    return (OGRFieldDefnShadow*) OGR_FD_GetFieldDefn(self, i);
  }

  int GetFieldIndex(const char* field_name) {
      // Do not issue an error if the field doesn't exist. It is intended to be silent
      return OGR_FD_GetFieldIndex(self, field_name);
  }

%apply Pointer NONNULL {OGRFieldDefnShadow* defn};
  void AddFieldDefn(OGRFieldDefnShadow* defn) {
    OGR_FD_AddFieldDefn(self, defn);
  }
%clear OGRFieldDefnShadow* defn;


  int GetGeomFieldCount(){
    return OGR_FD_GetGeomFieldCount(self);
  }

  /* FeatureDefns own their GeomFieldDefns */
  OGRGeomFieldDefnShadow* GetGeomFieldDefn(int i){
    return (OGRGeomFieldDefnShadow*) OGR_FD_GetGeomFieldDefn(self, i);
  }

  int GetGeomFieldIndex(const char* field_name) {
      // Do not issue an error if the field doesn't exist. It is intended to be silent
      return OGR_FD_GetGeomFieldIndex(self, field_name);
  }

%apply Pointer NONNULL {OGRGeomFieldDefnShadow* defn};
  void AddGeomFieldDefn(OGRGeomFieldDefnShadow* defn) {
    OGR_FD_AddGeomFieldDefn(self, defn);
  }
%clear OGRGeomFieldDefnShadow* defn;

  OGRErr DeleteGeomFieldDefn(int idx)
  {
    return OGR_FD_DeleteGeomFieldDefn(self, idx);
  }

  OGRwkbGeometryType GetGeomType() {
    return (OGRwkbGeometryType) OGR_FD_GetGeomType(self);
  }

  void SetGeomType(OGRwkbGeometryType geom_type) {
    if( ValidateOGRGeometryType(geom_type) )
        OGR_FD_SetGeomType(self, geom_type);
  }

  int GetReferenceCount(){
    return OGR_FD_GetReferenceCount(self);
  }

  int IsGeometryIgnored() {
    return OGR_FD_IsGeometryIgnored(self);
  }

  void SetGeometryIgnored( int bIgnored ) {
    return OGR_FD_SetGeometryIgnored(self,bIgnored);
  }

  int IsStyleIgnored() {
    return OGR_FD_IsStyleIgnored(self);
  }

  void SetStyleIgnored( int bIgnored ) {
    return OGR_FD_SetStyleIgnored(self,bIgnored);
  }

%apply Pointer NONNULL {OGRFeatureDefnShadow* other_defn};
  int IsSame(OGRFeatureDefnShadow* other_defn) {
    return OGR_FD_IsSame(self, other_defn);
  }
%clear OGRFeatureDefnShadow* other_defn;
} /* %extend */


}; /* class OGRFeatureDefnShadow */

/************************************************************************/
/*                             OGRFieldDefn                             */
/************************************************************************/

%rename (FieldDefn) OGRFieldDefnShadow;

%{
    static int ValidateOGRFieldType(OGRFieldType field_type)
    {
        switch(field_type)
        {
            case OFTInteger:
            case OFTIntegerList:
            case OFTReal:
            case OFTRealList:
            case OFTString:
            case OFTStringList:
            case OFTBinary:
            case OFTDate:
            case OFTTime:
            case OFTDateTime:
            case OFTInteger64:
            case OFTInteger64List:
                return TRUE;
            default:
                CPLError(CE_Failure, CPLE_IllegalArg, "Illegal field type value");
                return FALSE;
        }
    }
%}

%{
    static int ValidateOGRFieldSubType(OGRFieldSubType field_subtype)
    {
        switch(field_subtype)
        {
            case OFSTNone:
            case OFSTBoolean:
            case OFSTInt16:
            case OFSTFloat32:
            case OFSTJSON:
            case OFSTUUID:
                return TRUE;
            default:
                CPLError(CE_Failure, CPLE_IllegalArg, "Illegal field subtype value");
                return FALSE;
        }
    }
%}

class OGRFieldDefnShadow {
  OGRFieldDefnShadow();
public:
%extend {

  ~OGRFieldDefnShadow() {
    OGR_Fld_Destroy(self);
  }

#ifndef SWIGJAVA
  %feature("kwargs") OGRFieldDefnShadow;
#endif
#ifdef SWIGCSHARP
  %apply ( const char *utf8_path ) { (const char* name_null_ok) };
#endif
  OGRFieldDefnShadow( const char* name_null_ok="unnamed",
                      OGRFieldType field_type=OFTString) {
    if (ValidateOGRFieldType(field_type))
        return (OGRFieldDefnShadow*) OGR_Fld_Create(name_null_ok, field_type);
    else
        return NULL;
  }
#ifdef SWIGCSHARP
  %clear (const char* name_null_ok );
#endif

#ifdef SWIGCSHARP
  %apply ( const char *utf8_path ) { const char * GetName };
#endif
  const char * GetName() {
    return OGR_Fld_GetNameRef(self);
  }
#ifdef SWIGCSHARP
  %clear (const char * GetName );
#endif

#ifdef SWIGJAVA
  StringAsByteArray* GetNameAsByteArray() {
    return OGR_Fld_GetNameRef(self);
  }
#endif

  const char * GetNameRef() {
    return OGR_Fld_GetNameRef(self);
  }

#ifdef SWIGCSHARP
  %apply ( const char *utf8_path ) { (const char* name) };
#endif

  void SetName( const char* name) {
    OGR_Fld_SetName(self, name);
  }

#ifdef SWIGCSHARP
  %clear (const char* name );
#endif

  const char * GetAlternativeName() {
    return OGR_Fld_GetAlternativeNameRef(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetAlternativeNameAsByteArray() {
    return OGR_Fld_GetAlternativeNameRef(self);
  }
#endif

  const char * GetAlternativeNameRef() {
    return OGR_Fld_GetAlternativeNameRef(self);
  }

  void SetAlternativeName( const char* alternativeName) {
    OGR_Fld_SetAlternativeName(self, alternativeName);
  }

  OGRFieldType GetType() {
    return OGR_Fld_GetType(self);
  }

  void SetType(OGRFieldType type) {
    if (ValidateOGRFieldType(type))
        OGR_Fld_SetType(self, type);
  }

#ifdef SWIGJAVA
  // Alias for backward compatibility
  OGRFieldType GetFieldType() {
    return OGR_Fld_GetType(self);
  }
#endif

  OGRFieldSubType GetSubType() {
    return OGR_Fld_GetSubType(self);
  }

  void SetSubType(OGRFieldSubType type) {
    if (ValidateOGRFieldSubType(type))
        OGR_Fld_SetSubType(self, type);
  }

  OGRJustification GetJustify() {
    return OGR_Fld_GetJustify(self);
  }

  void SetJustify(OGRJustification justify) {
    OGR_Fld_SetJustify(self, justify);
  }

  int GetWidth () {
    return OGR_Fld_GetWidth(self);
  }

  void SetWidth (int width) {
    OGR_Fld_SetWidth(self, width);
  }

  int GetPrecision() {
    return OGR_Fld_GetPrecision(self);
  }

  void SetPrecision(int precision) {
    OGR_Fld_SetPrecision(self, precision);
  }

  int GetTZFlag() {
    return OGR_Fld_GetTZFlag(self);
  }

  void SetTZFlag(int tzflag) {
    OGR_Fld_SetTZFlag(self, tzflag);
  }

  /* Interface method added for GDAL 1.7.0 */
  const char * GetTypeName()
  {
      return OGR_GetFieldTypeName(OGR_Fld_GetType(self));
  }

  /* Should be static */
  const char * GetFieldTypeName(OGRFieldType type) {
    return OGR_GetFieldTypeName(type);
  }

  int IsIgnored() {
    return OGR_Fld_IsIgnored( self );
  }

  void SetIgnored(int bIgnored ) {
    OGR_Fld_SetIgnored( self, bIgnored );
  }

  int IsNullable() {
    return OGR_Fld_IsNullable( self );
  }

  void SetNullable(int bNullable ) {
    OGR_Fld_SetNullable( self, bNullable );
  }

  int IsUnique() {
    return OGR_Fld_IsUnique( self );
  }

  void SetUnique(int bUnique ) {
    OGR_Fld_SetUnique( self, bUnique );
  }

  int IsGenerated() {
    return OGR_Fld_IsGenerated( self );
  }

  void SetGenerated(int bGenerated ) {
    OGR_Fld_SetGenerated( self, bGenerated );
  }

  const char* GetDefault() {
    return OGR_Fld_GetDefault( self );
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetDefaultAsByteArray() {
    return OGR_Fld_GetDefault(self);
  }
#endif

  void SetDefault(const char* pszValue ) {
    OGR_Fld_SetDefault( self, pszValue );
  }

  int IsDefaultDriverSpecific() {
    return OGR_Fld_IsDefaultDriverSpecific( self );
  }

  const char* GetDomainName() {
    return OGR_Fld_GetDomainName(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetDomainNameAsByteArray() {
    return OGR_Fld_GetDomainName(self);
  }
#endif

  void SetDomainName(const char* name ) {
    OGR_Fld_SetDomainName( self, name );
  }

  const char* GetComment() {
    return OGR_Fld_GetComment(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetCommentAsByteArray() {
    return OGR_Fld_GetComment(self);
  }
#endif

  void SetComment(const char* comment ) {
    OGR_Fld_SetComment( self, comment );
  }
} /* %extend */


}; /* class OGRFieldDefnShadow */

/************************************************************************/
/*                          OGRGeomFieldDefn                            */
/************************************************************************/

%rename (GeomFieldDefn) OGRGeomFieldDefnShadow;

class OGRGeomFieldDefnShadow {
  OGRGeomFieldDefnShadow();
public:
%extend {

  ~OGRGeomFieldDefnShadow() {
    OGR_GFld_Destroy(self);
  }

#ifndef SWIGJAVA
  %feature("kwargs") OGRGeomFieldDefnShadow;
#endif
  OGRGeomFieldDefnShadow( const char* name_null_ok="",
                          OGRwkbGeometryType field_type = wkbUnknown) {
    if( ValidateOGRGeometryType(field_type) )
        return (OGRGeomFieldDefnShadow*) OGR_GFld_Create(name_null_ok, field_type);
    else
        return NULL;
  }

  const char * GetName() {
    return OGR_GFld_GetNameRef(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetNameAsByteArray() {
    return OGR_GFld_GetNameRef(self);
  }
#endif

  const char * GetNameRef() {
    return OGR_GFld_GetNameRef(self);
  }

  void SetName( const char* name) {
    OGR_GFld_SetName(self, name);
  }

  OGRwkbGeometryType GetType() {
    return OGR_GFld_GetType(self);
  }

  void SetType(OGRwkbGeometryType type) {
    if( ValidateOGRGeometryType(type) )
        OGR_GFld_SetType(self, type);
  }

  %newobject GetSpatialRef;
  OSRSpatialReferenceShadow *GetSpatialRef() {
    OGRSpatialReferenceH ref =  OGR_GFld_GetSpatialRef(self);
    if( ref )
        OSRReference(ref);
    return (OSRSpatialReferenceShadow*) ref;
  }

  void SetSpatialRef(OSRSpatialReferenceShadow* srs)
  {
     OGR_GFld_SetSpatialRef( self, (OGRSpatialReferenceH)srs );
  }

  int IsIgnored() {
    return OGR_GFld_IsIgnored( self );
  }

  void SetIgnored(int bIgnored ) {
    OGR_GFld_SetIgnored( self, bIgnored );
  }

  int IsNullable() {
    return OGR_GFld_IsNullable( self );
  }

  void SetNullable(int bNullable ) {
    return OGR_GFld_SetNullable( self, bNullable );
  }

  OGRGeomCoordinatePrecisionShadow* GetCoordinatePrecision() {
    return OGR_GFld_GetCoordinatePrecision(self);
  }

  %apply Pointer NONNULL {OGRGeomCoordinatePrecisionShadow* srs};
  void SetCoordinatePrecision(OGRGeomCoordinatePrecisionShadow* coordPrec) {
    OGR_GFld_SetCoordinatePrecision(self, coordPrec);
  }
  %clear OGRGeomCoordinatePrecisionShadow* srs;

} /* %extend */


}; /* class OGRGeomFieldDefnShadow */

/* -------------------------------------------------------------------- */
/*      Geometry factory methods.                                       */
/* -------------------------------------------------------------------- */

#ifndef SWIGJAVA
%feature( "kwargs" ) CreateGeometryFromWkb;
%newobject CreateGeometryFromWkb;
#ifndef SWIGCSHARP
%apply (size_t nLen, char *pBuf ) { (size_t len, char *bin_string)};
#else
%apply (void *buffer_ptr) {char *bin_string};
#endif
%inline %{
  OGRGeometryShadow* CreateGeometryFromWkb( size_t len, char *bin_string,
                                            OSRSpatialReferenceShadow *reference=NULL ) {
    OGRGeometryH geom = NULL;
    OGRErr err = OGR_G_CreateFromWkbEx( (unsigned char *) bin_string,
                                        reference,
                                        &geom,
                                        len );
    if (err != 0 ) {
       CPLError(CE_Failure, err, "%s", OGRErrMessages(err));
       return NULL;
    }
    return (OGRGeometryShadow*) geom;
  }

%}
#endif
#ifndef SWIGCSHARP
%clear (size_t len, char *bin_string);
#else
%clear (char *bin_string);
#endif

#ifdef SWIGJAVA
%newobject CreateGeometryFromWkb;
%inline {
OGRGeometryShadow* CreateGeometryFromWkb(int nLen, unsigned char *pBuf,
                                            OSRSpatialReferenceShadow *reference=NULL ) {
    OGRGeometryH geom = NULL;
    OGRErr err = OGR_G_CreateFromWkb((unsigned char*) pBuf, reference, &geom, nLen);
    if (err != 0 ) {
       CPLError(CE_Failure, err, "%s", OGRErrMessages(err));
       return NULL;
    }
    return (OGRGeometryShadow*) geom;
  }
}
#endif

#ifndef SWIGJAVA
%feature( "kwargs" ) CreateGeometryFromWkt;
#endif
%apply (char **ignorechange) { (char **) };
%newobject CreateGeometryFromWkt;
%inline %{
  OGRGeometryShadow* CreateGeometryFromWkt( char **val,
                                      OSRSpatialReferenceShadow *reference=NULL ) {
    OGRGeometryH geom = NULL;
    OGRErr err = OGR_G_CreateFromWkt(val,
                                      reference,
                                      &geom);
    if (err != 0 ) {
       CPLError(CE_Failure, err, "%s", OGRErrMessages(err));
       return NULL;
    }
    return (OGRGeometryShadow*) geom;
  }

%}
%clear (char **);

%newobject CreateGeometryFromGML;
%inline %{
  OGRGeometryShadow *CreateGeometryFromGML( const char * input_string ) {
    OGRGeometryShadow* geom = (OGRGeometryShadow*)OGR_G_CreateFromGML(input_string);
    return geom;
  }

%}

%newobject CreateGeometryFromJson;
%inline %{
  OGRGeometryShadow *CreateGeometryFromJson( const char * input_string ) {
    OGRGeometryShadow* geom = (OGRGeometryShadow*)OGR_G_CreateGeometryFromJson(input_string);
    return geom;
  }

%}

%newobject CreateGeometryFromEsriJson;
%inline %{
  OGRGeometryShadow *CreateGeometryFromEsriJson( const char * input_string ) {
    OGRGeometryShadow* geom = (OGRGeometryShadow*)OGR_G_CreateGeometryFromEsriJson(input_string);
    return geom;
  }

%}

#ifndef SWIGCSHARP
%newobject CreateGeometryFromEnvelope;
%inline %{
  OGRGeometryShadow *CreateGeometryFromEnvelope(double xmin, 
                                                double ymin, 
                                                double xmax,
                                                double ymax,
                                                OSRSpatialReferenceShadow *reference = nullptr) {
    OGRGeometryShadow* geom = (OGRGeometryShadow*) OGR_G_CreateFromEnvelope(xmin, ymin, xmax, ymax, reference);
    return geom;
  }
%}
#endif

%newobject BuildPolygonFromEdges;
#ifndef SWIGJAVA
%feature( "kwargs" ) BuildPolygonFromEdges;
#endif
%inline %{
  OGRGeometryShadow* BuildPolygonFromEdges( OGRGeometryShadow*  hLineCollection,
                                            int bBestEffort = 0,
                                            int bAutoClose = 0,
                                            double dfTolerance=0) {

  OGRGeometryH hPolygon = NULL;

  OGRErr eErr;

  hPolygon = OGRBuildPolygonFromEdges( hLineCollection, bBestEffort,
                                       bAutoClose, dfTolerance, &eErr );

  if (eErr != OGRERR_NONE ) {
    CPLError(CE_Failure, eErr, "%s", OGRErrMessages(eErr));
    return NULL;
  }

  return (OGRGeometryShadow* )hPolygon;
  }
%}

%newobject ApproximateArcAngles;
#ifndef SWIGJAVA
%feature( "kwargs" ) ApproximateArcAngles;
#endif
%inline %{
  OGRGeometryShadow* ApproximateArcAngles(
        double dfCenterX, double dfCenterY, double dfZ,
  	double dfPrimaryRadius, double dfSecondaryAxis, double dfRotation,
        double dfStartAngle, double dfEndAngle,
        double dfMaxAngleStepSizeDegrees ) {

  return (OGRGeometryShadow* )OGR_G_ApproximateArcAngles(
             dfCenterX, dfCenterY, dfZ,
             dfPrimaryRadius, dfSecondaryAxis, dfRotation,
             dfStartAngle, dfEndAngle, dfMaxAngleStepSizeDegrees );
  }
%}

%newobject ForceToPolygon;
/* Contrary to the C/C++ method, the passed geometry is preserved */
/* This avoids dirty trick for Java */
%inline %{
OGRGeometryShadow* ForceToPolygon( OGRGeometryShadow *geom_in ) {
 if (geom_in == NULL)
     return NULL;
 return (OGRGeometryShadow* )OGR_G_ForceToPolygon( OGR_G_Clone(geom_in) );
}
%}

%newobject ForceToLineString;
/* Contrary to the C/C++ method, the passed geometry is preserved */
/* This avoids dirty trick for Java */
%inline %{
OGRGeometryShadow* ForceToLineString( OGRGeometryShadow *geom_in ) {
 if (geom_in == NULL)
     return NULL;
 return (OGRGeometryShadow* )OGR_G_ForceToLineString( OGR_G_Clone(geom_in) );
}
%}

%newobject ForceToMultiPolygon;
/* Contrary to the C/C++ method, the passed geometry is preserved */
/* This avoids dirty trick for Java */
%inline %{
OGRGeometryShadow* ForceToMultiPolygon( OGRGeometryShadow *geom_in ) {
 if (geom_in == NULL)
     return NULL;
 return (OGRGeometryShadow* )OGR_G_ForceToMultiPolygon( OGR_G_Clone(geom_in) );
}
%}

%newobject ForceToMultiPoint;
/* Contrary to the C/C++ method, the passed geometry is preserved */
/* This avoids dirty trick for Java */
%inline %{
OGRGeometryShadow* ForceToMultiPoint( OGRGeometryShadow *geom_in ) {
 if (geom_in == NULL)
     return NULL;
 return (OGRGeometryShadow* )OGR_G_ForceToMultiPoint( OGR_G_Clone(geom_in) );
}
%}

%newobject ForceToMultiLineString;
/* Contrary to the C/C++ method, the passed geometry is preserved */
/* This avoids dirty trick for Java */
%inline %{
OGRGeometryShadow* ForceToMultiLineString( OGRGeometryShadow *geom_in ) {
 if (geom_in == NULL)
     return NULL;
 return (OGRGeometryShadow* )OGR_G_ForceToMultiLineString( OGR_G_Clone(geom_in) );
}
%}

%newobject ForceTo;
/* Contrary to the C/C++ method, the passed geometry is preserved */
/* This avoids dirty trick for Java */
%inline %{
OGRGeometryShadow* ForceTo( OGRGeometryShadow *geom_in, OGRwkbGeometryType eTargetType, char** options = NULL ) {
 if (geom_in == NULL)
     return NULL;
 return (OGRGeometryShadow* )OGR_G_ForceTo( OGR_G_Clone(geom_in), eTargetType, options );
}
%}

/************************************************************************/
/*                             OGRGeometry                              */
/************************************************************************/

%rename (Geometry) OGRGeometryShadow;
class OGRGeometryShadow {
  OGRGeometryShadow();
public:
%extend {

  ~OGRGeometryShadow() {
    OGR_G_DestroyGeometry( self );
  }

#ifndef SWIGJAVA
#ifdef SWIGCSHARP
%apply (void *buffer_ptr) {char *wkb_buf};
#else
%apply (int nLen, char *pBuf) {(int wkb, char *wkb_buf)};
#endif
  %feature("kwargs") OGRGeometryShadow;
  OGRGeometryShadow( OGRwkbGeometryType type = wkbUnknown, char *wkt = 0, int wkb = 0, char *wkb_buf = 0, char *gml = 0 ) {
    if (type != wkbUnknown ) {
      return (OGRGeometryShadow*) OGR_G_CreateGeometry( type );
    }
    else if ( wkt != 0 ) {
      return CreateGeometryFromWkt( &wkt );
    }
    else if ( wkb != 0 ) {
      return CreateGeometryFromWkb( wkb, wkb_buf );
    }
    else if ( gml != 0 ) {
      return CreateGeometryFromGML( gml );
    }
    // throw?
    else {
        CPLError(CE_Failure, 1, "Empty geometries cannot be constructed");
        return NULL;}

  }
#ifdef SWIGCSHARP
%clear (char *wkb_buf);
#else
%clear (int wkb, char *wkb_buf);
#endif
#endif

  OGRErr ExportToWkt( char** argout ) {
    return OGR_G_ExportToWkt(self, argout);
  }

  OGRErr ExportToIsoWkt( char** argout ) {
    return OGR_G_ExportToIsoWkt(self, argout);
  }

#ifndef SWIGCSHARP
#if defined(SWIGJAVA)
%apply (GByte* outBytes) {GByte*};
  GByte* ExportToWkb( size_t *nLen, char **pBuf, OGRwkbByteOrder byte_order=wkbNDR ) {
    *nLen = OGR_G_WkbSizeEx( self );
    *pBuf = (char *) VSI_MALLOC_VERBOSE( *nLen );
    if( *pBuf == NULL )
        return NULL;
    OGR_G_ExportToWkb(self, byte_order, (unsigned char*) *pBuf );
    return (GByte*)*pBuf;
  }

  GByte* ExportToIsoWkb( size_t *nLen, char **pBuf, OGRwkbByteOrder byte_order=wkbNDR ) {
    *nLen = OGR_G_WkbSizeEx( self );
    *pBuf = (char *) VSI_MALLOC_VERBOSE( *nLen );
    if( *pBuf == NULL )
        return NULL;
    OGR_G_ExportToIsoWkb(self, byte_order, (unsigned char*) *pBuf );
    return (GByte*)*pBuf;
  }
%clear GByte*;
#elif defined(SWIGPYTHON)
  %feature("kwargs") ExportToWkb;
  OGRErr ExportToWkb( size_t *nLen, char **pBuf, OGRwkbByteOrder byte_order=wkbNDR ) {
    *nLen = OGR_G_WkbSizeEx( self );
    *pBuf = (char *) VSI_MALLOC_VERBOSE( *nLen );
    if( *pBuf == NULL )
        return OGRERR_FAILURE;
    return OGR_G_ExportToWkb(self, byte_order, (unsigned char*) *pBuf );
  }

  %feature("kwargs") ExportToIsoWkb;
  OGRErr ExportToIsoWkb( size_t *nLen, char **pBuf, OGRwkbByteOrder byte_order=wkbNDR ) {
    *nLen = OGR_G_WkbSizeEx( self );
    *pBuf = (char *) VSI_MALLOC_VERBOSE( *nLen );
    if( *pBuf == NULL )
        return OGRERR_FAILURE;
    return OGR_G_ExportToIsoWkb(self, byte_order, (unsigned char*) *pBuf );
  }
#else
  %feature("kwargs") ExportToWkb;
  OGRErr ExportToWkb( int *nLen, char **pBuf, OGRwkbByteOrder byte_order=wkbNDR ) {
    *nLen = OGR_G_WkbSize( self );
    *pBuf = (char *) VSI_MALLOC_VERBOSE( *nLen );
    if( *pBuf == NULL )
        return OGRERR_FAILURE;
    return OGR_G_ExportToWkb(self, byte_order, (unsigned char*) *pBuf );
  }

  %feature("kwargs") ExportToIsoWkb;
  OGRErr ExportToIsoWkb( int *nLen, char **pBuf, OGRwkbByteOrder byte_order=wkbNDR ) {
    *nLen = OGR_G_WkbSize( self );
    *pBuf = (char *) VSI_MALLOC_VERBOSE( *nLen );
    if( *pBuf == NULL )
        return OGRERR_FAILURE;
    return OGR_G_ExportToIsoWkb(self, byte_order, (unsigned char*) *pBuf );
  }
#endif
#endif

#if defined(SWIGCSHARP)
  retStringAndCPLFree* ExportToGML() {
    return (retStringAndCPLFree*) OGR_G_ExportToGMLEx(self, NULL);
  }

  retStringAndCPLFree* ExportToGML(char** options) {
    return (retStringAndCPLFree*) OGR_G_ExportToGMLEx(self, options);
  }
#elif defined(SWIGJAVA) || defined(SWIGPYTHON)
#ifndef SWIGJAVA
  %feature("kwargs") ExportToGML;
#endif
  retStringAndCPLFree* ExportToGML(char** options=0) {
    return (retStringAndCPLFree*) OGR_G_ExportToGMLEx(self, options);
  }
#else
  /* FIXME : wrong typemap. The string should be freed */
  const char * ExportToGML() {
    return (const char *) OGR_G_ExportToGML(self);
  }
#endif

#if defined(SWIGJAVA) || defined(SWIGPYTHON) || defined(SWIGCSHARP)
  retStringAndCPLFree* ExportToKML(const char* altitude_mode=NULL) {
    return (retStringAndCPLFree *) OGR_G_ExportToKML(self, altitude_mode);
  }
#else
  /* FIXME : wrong typemap. The string should be freed */
  const char * ExportToKML(const char* altitude_mode=NULL) {
    return (const char *) OGR_G_ExportToKML(self, altitude_mode);
  }
#endif

#if defined(SWIGJAVA) || defined(SWIGPYTHON) || defined(SWIGCSHARP)
#ifndef SWIGJAVA
  %feature("kwargs") ExportToJson;
#endif
  retStringAndCPLFree* ExportToJson(char** options=0) {
    return (retStringAndCPLFree *) OGR_G_ExportToJsonEx(self, options);
  }
#else
  /* FIXME : wrong typemap. The string should be freed */
  const char * ExportToJson() {
    return (const char *) OGR_G_ExportToJson(self);
  }
#endif

#ifndef SWIGJAVA
  %feature("kwargs") AddPoint;
#endif
  void AddPoint(double x, double y, double z = 0) {
    OGR_G_AddPoint( self, x, y, z );
  }

#ifndef SWIGJAVA
  %feature("kwargs") AddPointM;
#endif
  void AddPointM(double x, double y, double m) {
      OGR_G_AddPointM( self, x, y, m );
  }

#ifndef SWIGJAVA
  %feature("kwargs") AddPointZM;
#endif
  void AddPointZM(double x, double y, double z, double m) {
      OGR_G_AddPointZM( self, x, y, z, m );
  }

  void AddPoint_2D(double x, double y) {
    OGR_G_AddPoint_2D( self, x, y );
  }

/* The geometry now owns an inner geometry */
/* Don't change the 'other_disown' name as Java bindings depends on it */
%apply SWIGTYPE *DISOWN {OGRGeometryShadow* other_disown};
%apply Pointer NONNULL {OGRGeometryShadow* other_disown};
  OGRErr AddGeometryDirectly( OGRGeometryShadow* other_disown ) {
    return OGR_G_AddGeometryDirectly( self, other_disown );
  }
%clear OGRGeometryShadow* other_disown;

%apply Pointer NONNULL {OGRGeometryShadow* other};
  OGRErr AddGeometry( OGRGeometryShadow* other ) {
    return OGR_G_AddGeometry( self, other );
  }
%clear OGRGeometryShadow* other;

  OGRErr RemoveGeometry( int iSubGeom ) {
    return OGR_G_RemoveGeometry( self, iSubGeom, TRUE );
  }

  %newobject Clone;
  OGRGeometryShadow* Clone() {
    return (OGRGeometryShadow*) OGR_G_Clone(self);
  }

  OGRwkbGeometryType GetGeometryType() {
    return (OGRwkbGeometryType) OGR_G_GetGeometryType(self);
  }

  const char * GetGeometryName() {
    return (const char *) OGR_G_GetGeometryName(self);
  }

  double Length () {
    return OGR_G_Length(self);
  }

  double Area() {
    return OGR_G_Area(self);
  }

  double GeodesicLength() {
    return OGR_G_GeodesicLength(self);
  }

  double GeodesicArea() {
    return OGR_G_GeodesicArea(self);
  }

  bool IsClockwise() {
    return OGR_G_IsClockwise(self);
  }

  /* old, non-standard API */
  double GetArea() {
    return OGR_G_Area(self);
  }

  int GetPointCount() {
    return OGR_G_GetPointCount(self);
  }

  /* since GDAL 1.9.0 */
#if defined(SWIGPYTHON) || defined(SWIGJAVA)
#ifdef SWIGJAVA
  retGetPoints* GetPoints(int* pnCount, double** ppadfXY, double** ppadfZ, int nCoordDimension = 0)
  {
    int nPoints = OGR_G_GetPointCount(self);
    *pnCount = nPoints;
    if (nPoints == 0)
    {
        *ppadfXY = NULL;
        *ppadfZ = NULL;
    }
    *ppadfXY = (double*)VSIMalloc(2 * sizeof(double) * nPoints);
    if (*ppadfXY == NULL)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory, "Cannot allocate resulting array");
        *pnCount = 0;
        return NULL;
    }
    if (nCoordDimension <= 0)
        nCoordDimension = OGR_G_GetCoordinateDimension(self);
    *ppadfZ = (nCoordDimension == 3) ? (double*)VSIMalloc(sizeof(double) * nPoints) : NULL;
    OGR_G_GetPoints(self,
                    *ppadfXY, 2 * sizeof(double),
                    (*ppadfXY) + 1, 2 * sizeof(double),
                    *ppadfZ, sizeof(double));
    return NULL;
  }
#else
  %feature("kwargs") GetPoints;
  void GetPoints(int* pnCount, double** ppadfXY, double** ppadfZ, int nCoordDimension = 0)
  {
    int nPoints = OGR_G_GetPointCount(self);
    *pnCount = nPoints;
    if (nPoints == 0)
    {
        *ppadfXY = NULL;
        *ppadfZ = NULL;
    }
    *ppadfXY = (double*)VSIMalloc(2 * sizeof(double) * nPoints);
    if (*ppadfXY == NULL)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory, "Cannot allocate resulting array");
        *pnCount = 0;
        return;
    }
    if (nCoordDimension <= 0)
        nCoordDimension = OGR_G_GetCoordinateDimension(self);
    *ppadfZ = (nCoordDimension == 3) ? (double*)VSIMalloc(sizeof(double) * nPoints) : NULL;
    OGR_G_GetPoints(self,
                    *ppadfXY, 2 * sizeof(double),
                    (*ppadfXY) + 1, 2 * sizeof(double),
                    *ppadfZ, sizeof(double));
  }
#endif
#endif

#ifndef SWIGJAVA
  %feature("kwargs") GetX;
#endif
  double GetX(int point=0) {
    return OGR_G_GetX(self, point);
  }

#ifndef SWIGJAVA
  %feature("kwargs") GetY;
#endif
  double GetY(int point=0) {
    return OGR_G_GetY(self, point);
  }

#ifndef SWIGJAVA
  %feature("kwargs") GetZ;
#endif
  double GetZ(int point=0) {
    return OGR_G_GetZ(self, point);
  }

#ifndef SWIGJAVA
  %feature("kwargs") GetM;
#endif
  double GetM(int point=0) {
    return OGR_G_GetM(self, point);
  }

#ifdef SWIGJAVA
  void GetPoint(int iPoint, double argout[3]) {
#else
  void GetPoint(int iPoint = 0, double argout[3] = NULL) {
#endif
    OGR_G_GetPoint( self, iPoint, argout+0, argout+1, argout+2 );
  }

#ifdef SWIGJAVA
  void GetPointZM(int iPoint, double argout[4]) {
#else
  void GetPointZM(int iPoint = 0, double argout[4] = NULL) {
#endif
      OGR_G_GetPointZM( self, iPoint, argout+0, argout+1, argout+2, argout+3 );
  }

#ifdef SWIGJAVA
  void GetPoint_2D(int iPoint, double argout[2]) {
#else
  void GetPoint_2D(int iPoint = 0, double argout[2] = NULL) {
#endif
    OGR_G_GetPoint( self, iPoint, argout+0, argout+1, NULL );
  }

  int GetGeometryCount() {
    return OGR_G_GetGeometryCount(self);
  }

#ifndef SWIGJAVA
  %feature("kwargs") SetPoint;
#endif
  void SetPoint(int point, double x, double y, double z=0) {
    OGR_G_SetPoint(self, point, x, y, z);
  }

#ifndef SWIGJAVA
  %feature("kwargs") SetPointM;
#endif
  void SetPointM(int point, double x, double y, double m) {
      OGR_G_SetPointM(self, point, x, y, m);
  }

#ifndef SWIGJAVA
  %feature("kwargs") SetPointZM;
#endif
  void SetPointZM(int point, double x, double y, double z, double m) {
      OGR_G_SetPointZM(self, point, x, y, z, m);
  }

#ifndef SWIGJAVA
  %feature("kwargs") SetPoint_2D;
#endif
  void SetPoint_2D(int point, double x, double y) {
    OGR_G_SetPoint_2D(self, point, x, y);
  }

  /* OGR >= 2.3 */
  void SwapXY() {
    OGR_G_SwapXY(self);
  }

  /* Geometries own their internal geometries */
  OGRGeometryShadow* GetGeometryRef(int geom) {
    return (OGRGeometryShadow*) OGR_G_GetGeometryRef(self, geom);
  }

  %newobject Simplify;
  OGRGeometryShadow* Simplify(double tolerance) {
    return (OGRGeometryShadow*) OGR_G_Simplify(self, tolerance);
  }

  /* OGR >= 1.9.0 */
  %newobject SimplifyPreserveTopology;
  OGRGeometryShadow* SimplifyPreserveTopology(double tolerance) {
    return (OGRGeometryShadow*) OGR_G_SimplifyPreserveTopology(self, tolerance);
  }

  /* OGR >= 2.1 */
  %newobject DelaunayTriangulation;
#ifndef SWIGJAVA
  %feature("kwargs") DelaunayTriangulation;
#endif
  OGRGeometryShadow* DelaunayTriangulation(double dfTolerance = 0.0, int bOnlyEdges = FALSE) {
    return (OGRGeometryShadow*) OGR_G_DelaunayTriangulation(self, dfTolerance, bOnlyEdges);
  }

  %newobject Polygonize;
  OGRGeometryShadow* Polygonize() {
    return (OGRGeometryShadow*) OGR_G_Polygonize(self);
  }

  %newobject BuildArea;
  OGRGeometryShadow* BuildArea() {
    return (OGRGeometryShadow*) OGR_G_BuildArea(self);
  }

  %newobject Boundary;
  OGRGeometryShadow* Boundary() {
    return (OGRGeometryShadow*) OGR_G_Boundary(self);
  }

  %newobject GetBoundary;
  OGRGeometryShadow* GetBoundary() {
    return (OGRGeometryShadow*) OGR_G_Boundary(self);
  }

  %newobject ConvexHull;
  OGRGeometryShadow* ConvexHull() {
    return (OGRGeometryShadow*) OGR_G_ConvexHull(self);
  }

  %newobject ConcaveHull;
  OGRGeometryShadow* ConcaveHull(double ratio, bool allowHoles) {
    return (OGRGeometryShadow*) OGR_G_ConcaveHull(self, ratio, allowHoles);
  }

  %newobject MakeValid;
  OGRGeometryShadow* MakeValid( char** options = NULL ) {
    return (OGRGeometryShadow*) OGR_G_MakeValidEx(self, options);
  }

  %newobject SetPrecision;
  OGRGeometryShadow* SetPrecision(double gridSize, int flags = 0) {
    return (OGRGeometryShadow*) OGR_G_SetPrecision(self, gridSize, flags);
  }

  %newobject Normalize;
  OGRGeometryShadow* Normalize() {
    return (OGRGeometryShadow*) OGR_G_Normalize(self);
  }

  %newobject RemoveLowerDimensionSubGeoms;
  OGRGeometryShadow* RemoveLowerDimensionSubGeoms() {
    return (OGRGeometryShadow*) OGR_G_RemoveLowerDimensionSubGeoms(self);
  }

  %newobject Buffer;
#if !defined(SWIGJAVA) && !defined(SWIGPYTHON)
  %feature("kwargs") Buffer;
#endif
  OGRGeometryShadow* Buffer( double distance, int quadsecs=30 ) {
    return (OGRGeometryShadow*) OGR_G_Buffer( self, distance, quadsecs );
  }

  OGRGeometryShadow* Buffer( double distance, char** options ) {
    return (OGRGeometryShadow*) OGR_G_BufferEx( self, distance, options );
  }

%apply Pointer NONNULL {OGRGeometryShadow* other};
  %newobject Intersection;
  OGRGeometryShadow* Intersection( OGRGeometryShadow* other ) {
    return (OGRGeometryShadow*) OGR_G_Intersection( self, other );
  }

  %newobject Union;
  OGRGeometryShadow* Union( OGRGeometryShadow* other ) {
    return (OGRGeometryShadow*) OGR_G_Union( self, other );
  }

  %newobject UnionCascaded;
  OGRGeometryShadow* UnionCascaded() {
    return (OGRGeometryShadow*) OGR_G_UnionCascaded( self );
  }

  %newobject UnaryUnion;
  OGRGeometryShadow* UnaryUnion() {
    return (OGRGeometryShadow*) OGR_G_UnaryUnion( self );
  }

  %newobject Difference;
  OGRGeometryShadow* Difference( OGRGeometryShadow* other ) {
    return (OGRGeometryShadow*) OGR_G_Difference( self, other );
  }

  %newobject SymDifference;
  OGRGeometryShadow* SymDifference( OGRGeometryShadow* other ) {
    return (OGRGeometryShadow*) OGR_G_SymDifference( self, other );
  }

  /* old, non-standard API */
  %newobject SymmetricDifference;
  OGRGeometryShadow* SymmetricDifference( OGRGeometryShadow* other ) {
    return (OGRGeometryShadow*) OGR_G_SymDifference( self, other );
  }

  double Distance( OGRGeometryShadow* other) {
    return OGR_G_Distance(self, other);
  }

  double Distance3D( OGRGeometryShadow* other) {
    return OGR_G_Distance3D(self, other);
  }
%clear OGRGeometryShadow* other;

  void Empty () {
    OGR_G_Empty(self);
  }

  bool IsEmpty () {
    return (OGR_G_IsEmpty(self) > 0);
  }

  bool IsValid () {
    return (OGR_G_IsValid(self) > 0);
  }

  bool IsSimple () {
    return (OGR_G_IsSimple(self) > 0);
  }

  bool IsRing () {
    return (OGR_G_IsRing(self) > 0);
  }

%apply Pointer NONNULL {OGRGeometryShadow* other};

  bool Intersects (OGRGeometryShadow* other) {
    return (OGR_G_Intersects(self, other) > 0);
  }

  /* old, non-standard API */
  bool Intersect (OGRGeometryShadow* other) {
    return (OGR_G_Intersects(self, other) > 0);
  }

  bool Equals (OGRGeometryShadow* other) {
    return (OGR_G_Equals(self, other) > 0);
  }

  /* old, non-standard API */
  bool Equal (OGRGeometryShadow* other) {
    return (OGR_G_Equals(self, other) > 0);
  }

  bool Disjoint(OGRGeometryShadow* other) {
    return (OGR_G_Disjoint(self, other) > 0);
  }

  bool Touches (OGRGeometryShadow* other) {
    return (OGR_G_Touches(self, other) > 0);
  }

  bool Crosses (OGRGeometryShadow* other) {
    return (OGR_G_Crosses(self, other) > 0);
  }

  bool Within (OGRGeometryShadow* other) {
    return (OGR_G_Within(self, other) > 0);
  }

  bool Contains (OGRGeometryShadow* other) {
    return (OGR_G_Contains(self, other) > 0);
  }

  bool Overlaps (OGRGeometryShadow* other) {
    return (OGR_G_Overlaps(self, other) > 0);
  }
%clear OGRGeometryShadow* other;

%apply Pointer NONNULL {OSRSpatialReferenceShadow* reference};
  OGRErr TransformTo(OSRSpatialReferenceShadow* reference) {
    return OGR_G_TransformTo(self, reference);
  }
%clear OSRSpatialReferenceShadow* reference;

%apply Pointer NONNULL {OSRCoordinateTransformationShadow* trans};
  OGRErr Transform(OSRCoordinateTransformationShadow* trans) {
    return OGR_G_Transform(self, trans);
  }
%clear OSRCoordinateTransformationShadow* trans;

  %newobject GetSpatialReference;
  OSRSpatialReferenceShadow* GetSpatialReference() {
    OGRSpatialReferenceH ref =  OGR_G_GetSpatialReference(self);
    if( ref )
        OSRReference(ref);
    return (OSRSpatialReferenceShadow*) ref;
  }

  void AssignSpatialReference(OSRSpatialReferenceShadow* reference) {
    OGR_G_AssignSpatialReference(self, reference);
  }

  void CloseRings() {
    OGR_G_CloseRings(self);
  }

  void FlattenTo2D() {
    OGR_G_FlattenTo2D(self);
  }

  void Segmentize(double dfMaxLength) {
    OGR_G_Segmentize(self, dfMaxLength);
  }


#if defined(SWIGCSHARP)
  void GetEnvelope(OGREnvelope *env) {
    OGR_G_GetEnvelope(self, env);
  }

  void GetEnvelope3D(OGREnvelope3D *env) {
    OGR_G_GetEnvelope3D(self, env);
  }
#else
  void GetEnvelope(double argout[4]) {
    OGR_G_GetEnvelope(self, (OGREnvelope*)argout);
  }

  void GetEnvelope3D(double argout[6]) {
    OGR_G_GetEnvelope3D(self, (OGREnvelope3D*)argout);
  }
#endif

  %newobject Centroid;
  OGRGeometryShadow* Centroid() {
    OGRGeometryShadow *pt = (OGRGeometryShadow*) OGR_G_CreateGeometry( wkbPoint );
    OGR_G_Centroid( self, pt );
    return pt;
  }

  %newobject PointOnSurface;
  OGRGeometryShadow* PointOnSurface() {
    return (OGRGeometryShadow*) OGR_G_PointOnSurface( self );
  }

  size_t WkbSize() {
    return OGR_G_WkbSizeEx(self);
  }

  int GetCoordinateDimension() {
    return OGR_G_GetCoordinateDimension(self);
  }

  int CoordinateDimension() {
    return OGR_G_CoordinateDimension(self);
  }

  int Is3D() {
      return OGR_G_Is3D(self);
  }

  int IsMeasured() {
      return OGR_G_IsMeasured(self);
  }

  void SetCoordinateDimension(int dimension) {
    OGR_G_SetCoordinateDimension(self, dimension);
  }

  void Set3D(int b3D) {
      OGR_G_Set3D(self, b3D);
  }

  void SetMeasured(int bMeasured) {
      OGR_G_SetMeasured(self, bMeasured);
  }

  int GetDimension() {
    return OGR_G_GetDimension(self);
  }

  int HasCurveGeometry(int bLookForCircular = FALSE)
  {
        return OGR_G_HasCurveGeometry(self, bLookForCircular);
  }

  %newobject GetLinearGeometry;
#ifndef SWIGJAVA
  %feature("kwargs") GetLinearGeometry;
#endif
  OGRGeometryShadow* GetLinearGeometry(double dfMaxAngleStepSizeDegrees = 0.0,char** options = NULL) {
    return (OGRGeometryShadow* )OGR_G_GetLinearGeometry(self, dfMaxAngleStepSizeDegrees, options);
  }

  %newobject GetCurveGeometry;
#ifndef SWIGJAVA
  %feature("kwargs") GetCurveGeometry;
#endif
  OGRGeometryShadow* GetCurveGeometry(char** options = NULL) {
    return (OGRGeometryShadow* )OGR_G_GetCurveGeometry(self, options);
  }

  %newobject Value;
  OGRGeometryShadow* Value(double dfDistance) {
    return (OGRGeometryShadow*)OGR_G_Value(self, dfDistance);
  }

  %newobject Transform;
  %apply Pointer NONNULL {OGRGeomTransformerShadow* transformer};
  OGRGeometryShadow* Transform(OGRGeomTransformerShadow* transformer)
  {
    return (OGRGeometryShadow*)OGR_GeomTransformer_Transform(transformer, self);
  }

  %newobject CreatePreparedGeometry;
  OGRPreparedGeometryShadow* CreatePreparedGeometry()
  {
    return (OGRPreparedGeometryShadow*)OGRCreatePreparedGeometry(self);
  }
} /* %extend */

}; /* class OGRGeometryShadow */


/************************************************************************/
/*                        OGRPreparedGeometry                           */
/************************************************************************/

%rename (PreparedGeometry) OGRPreparedGeometryShadow;
class OGRPreparedGeometryShadow {
  OGRPreparedGeometryShadow();
public:
%extend {

  ~OGRPreparedGeometryShadow() {
    OGRDestroyPreparedGeometry( self );
  }

  %apply Pointer NONNULL {const OGRGeometryShadow* otherGeom};
  bool Intersects(const OGRGeometryShadow* otherGeom) {
    return OGRPreparedGeometryIntersects(self, (OGRGeometryH)otherGeom);
  }

  bool Contains(const OGRGeometryShadow* otherGeom) {
    return OGRPreparedGeometryContains(self, (OGRGeometryH)otherGeom);
  }

} /* %extend */

}; /* class OGRPreparedGeometryShadow */


#ifdef SWIGPYTHON
/* Applies perhaps to other bindings */
%clear (const char* field_name);
#endif

/************************************************************************/
/*                         OGRGeomTransformerH                          */
/************************************************************************/

%rename (GeomTransformer) OGRGeomTransformerShadow;
class OGRGeomTransformerShadow {
  OGRGeomTransformerShadow();
public:
%extend {

  OGRGeomTransformerShadow(OSRCoordinateTransformationShadow* ct,
                           char** options = NULL ) {
    return OGR_GeomTransformer_Create(ct, options);
  }

  ~OGRGeomTransformerShadow() {
    OGR_GeomTransformer_Destroy( self );
  }

  %newobject Transform;
  %apply Pointer NONNULL {OGRGeometryShadow* src_geom};
  OGRGeometryShadow* Transform(OGRGeometryShadow* src_geom)
  {
    return (OGRGeometryShadow*)OGR_GeomTransformer_Transform(self, src_geom);
  }
} /* %extend */

}; /* class OGRGeomTransformerShadow */

/************************************************************************/
/*                          OGRFieldDomain                              */
/************************************************************************/

%rename (FieldDomain) OGRFieldDomainShadow;

class OGRFieldDomainShadow {
  OGRFieldDomainShadow();
public:
%extend {

  ~OGRFieldDomainShadow() {
    OGR_FldDomain_Destroy(self);
  }

  const char * GetName() {
    return OGR_FldDomain_GetName(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetNameAsByteArray() {
    return OGR_FldDomain_GetName(self);
  }
#endif

  const char * GetDescription() {
    return OGR_FldDomain_GetDescription(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetDescriptionAsByteArray() {
    return OGR_FldDomain_GetDescription(self);
  }
#endif

  OGRFieldType GetFieldType() {
    return OGR_FldDomain_GetFieldType(self);
  }

  OGRFieldSubType GetFieldSubType() {
    return OGR_FldDomain_GetFieldSubType(self);
  }

  OGRFieldDomainType GetDomainType() {
    return OGR_FldDomain_GetDomainType(self);
  }

  OGRFieldDomainSplitPolicy GetSplitPolicy() {
    return OGR_FldDomain_GetSplitPolicy(self);
  }

  void SetSplitPolicy(OGRFieldDomainSplitPolicy policy) {
    OGR_FldDomain_SetSplitPolicy(self, policy);
  }

  OGRFieldDomainMergePolicy GetMergePolicy() {
    return OGR_FldDomain_GetMergePolicy(self);
  }

  void SetMergePolicy(OGRFieldDomainMergePolicy policy) {
    OGR_FldDomain_SetMergePolicy(self, policy);
  }

#if defined(SWIGPYTHON) || defined(SWIGJAVA)
  const OGRCodedValue* GetEnumeration() {
    return OGR_CodedFldDomain_GetEnumeration(self);
  }
#endif

  double GetMinAsDouble() {
      const OGRField* psVal = OGR_RangeFldDomain_GetMin(self, NULL);
      if( psVal == NULL || OGR_RawField_IsUnset(psVal) )
          return CPLAtof("-inf");
      const OGRFieldType eType = OGR_FldDomain_GetFieldType(self);
      if( eType == OFTInteger )
          return psVal->Integer;
      if( eType == OFTInteger64 )
          return (double)psVal->Integer64;
      if( eType == OFTReal )
          return psVal->Real;
      return CPLAtof("-inf");
  }

  const char* GetMinAsString() {
    const OGRField* psVal = OGR_RangeFldDomain_GetMin(self, NULL);
      if( psVal == NULL || OGR_RawField_IsUnset(psVal) )
          return NULL;
      const OGRFieldType eType = OGR_FldDomain_GetFieldType(self);
      if( eType == OFTInteger )
          return CPLSPrintf("%d", psVal->Integer);
      if( eType == OFTInteger64 )
          return CPLSPrintf(CPL_FRMT_GIB, psVal->Integer64);
      if( eType == OFTReal )
          return CPLSPrintf("%.18g", psVal->Real);
      if( eType == OFTDateTime )
          return CPLSPrintf("%04d-%02d-%02dT%02d:%02d:%02d",
                     psVal->Date.Year,
                     psVal->Date.Month,
                     psVal->Date.Day,
                     psVal->Date.Hour,
                     psVal->Date.Minute,
                     static_cast<int>(psVal->Date.Second + 0.5));
     return NULL;
  }

  bool IsMinInclusive() {
      bool isInclusive = false;
      (void)OGR_RangeFldDomain_GetMin(self, &isInclusive);
      return isInclusive;
  }

  double GetMaxAsDouble() {
      const OGRField* psVal = OGR_RangeFldDomain_GetMax(self, NULL);
      if( psVal == NULL || OGR_RawField_IsUnset(psVal) )
          return CPLAtof("inf");
      const OGRFieldType eType = OGR_FldDomain_GetFieldType(self);
      if( eType == OFTInteger )
          return psVal->Integer;
      if( eType == OFTInteger64 )
          return (double)psVal->Integer64;
      if( eType == OFTReal )
          return psVal->Real;
      return CPLAtof("inf");
  }

  const char* GetMaxAsString() {
    const OGRField* psVal = OGR_RangeFldDomain_GetMax(self, NULL);
      if( psVal == NULL || OGR_RawField_IsUnset(psVal) )
          return NULL;
      const OGRFieldType eType = OGR_FldDomain_GetFieldType(self);
      if( eType == OFTInteger )
          return CPLSPrintf("%d", psVal->Integer);
      if( eType == OFTInteger64 )
          return CPLSPrintf(CPL_FRMT_GIB, psVal->Integer64);
      if( eType == OFTReal )
          return CPLSPrintf("%.18g", psVal->Real);
      if( eType == OFTDateTime )
          return CPLSPrintf("%04d-%02d-%02dT%02d:%02d:%02d",
                     psVal->Date.Year,
                     psVal->Date.Month,
                     psVal->Date.Day,
                     psVal->Date.Hour,
                     psVal->Date.Minute,
                     static_cast<int>(psVal->Date.Second + 0.5));
     return NULL;
  }

  bool IsMaxInclusive() {
      bool isInclusive = false;
      (void)OGR_RangeFldDomain_GetMax(self, &isInclusive);
      return isInclusive;
  }

  const char* GetGlob() {
      return OGR_GlobFldDomain_GetGlob(self);
  }

#ifdef SWIGJAVA
  StringAsByteArray* GetGlobAsByteArray() {
    return OGR_GlobFldDomain_GetGlob(self);
  }
#endif

} /* %extend */

}; /* class OGRFieldDomainShadow */

#if defined(SWIGPYTHON) || defined(SWIGJAVA)
%newobject CreateCodedFieldDomain;
%apply Pointer NONNULL {const char* name};
%inline %{
static
OGRFieldDomainShadow* CreateCodedFieldDomain( const char *name,
                                              const char* description,
                                              OGRFieldType type,
                                              OGRFieldSubType subtype,
                                              const OGRCodedValue* enumeration) {
  return (OGRFieldDomainShadow*) OGR_CodedFldDomain_Create( name,
                                                            description,
                                                            type,
                                                            subtype,
                                                            enumeration );
}
%}
%clear const char* name;
#endif

%newobject CreateRangeFieldDomain;
%apply Pointer NONNULL {const char* name};

#ifdef SWIGPYTHON
%apply (double *optional_double) {(double*)};

%inline %{
static
OGRFieldDomainShadow* CreateRangeFieldDomain( const char *name,
                                              const char* description,
                                              OGRFieldType type,
                                              OGRFieldSubType subtype,
                                              double* min,
                                              bool minIsInclusive,
                                              double* max,
                                              bool maxIsInclusive) {
  OGRField sMin;
  if (min )
  {
      if( type == OFTInteger )
          sMin.Integer = static_cast<int>(*min);
      else if( type == OFTInteger64 )
          sMin.Integer64 = static_cast<GIntBig>(*min);
      else if( type == OFTReal )
          sMin.Real = *min;
      else
          return NULL;
  }

  OGRField sMax;
  if( max )
  {
      if( type == OFTInteger )
          sMax.Integer = static_cast<int>(*max);
      else if( type == OFTInteger64 )
          sMax.Integer64 = static_cast<GIntBig>(*max);
      else if( type == OFTReal )
          sMax.Real = *max;
      else
          return NULL;
  }
  return (OGRFieldDomainShadow*) OGR_RangeFldDomain_Create( name,
                                                            description,
                                                            type,
                                                            subtype,
                                                            min ? &sMin : NULL,
                                                            minIsInclusive,
                                                            max ? &sMax : NULL,
                                                            maxIsInclusive );
}
%}

#else

%inline %{
static
OGRFieldDomainShadow* CreateRangeFieldDomain( const char *name,
                                              const char* description,
                                              OGRFieldType type,
                                              OGRFieldSubType subtype,
                                              double min,
                                              bool minIsInclusive,
                                              double max,
                                              bool maxIsInclusive) {
  OGRField sMin;
  if( type == OFTInteger )
      sMin.Integer = static_cast<int>(min);
  else if( type == OFTInteger64 )
      sMin.Integer64 = static_cast<GIntBig>(min);
  else if( type == OFTReal )
      sMin.Real = min;
  else
      return NULL;
  OGRField sMax;
  if( type == OFTInteger )
      sMax.Integer = static_cast<int>(max);
  else if( type == OFTInteger64 )
      sMax.Integer64 = static_cast<GIntBig>(max);
  else if( type == OFTReal )
      sMax.Real = max;
  else
      return NULL;
  return (OGRFieldDomainShadow*) OGR_RangeFldDomain_Create( name,
                                                            description,
                                                            type,
                                                            subtype,
                                                            &sMin,
                                                            minIsInclusive,
                                                            &sMax,
                                                            maxIsInclusive );
}
%}

#endif

%inline %{
static
OGRFieldDomainShadow* CreateRangeFieldDomainDateTime( const char *name,
                                              const char* description,
                                              const char* min,
                                              bool minIsInclusive,
                                              const char* max,
                                              double maxIsInclusive) {
  OGRField sMin;
  OGRField sMax;
  if( min && !OGRParseXMLDateTime(min, &sMin))
  {
    CPLError(CE_Failure, CPLE_AppDefined,
             "Invalid min: %s",
             min);
    return NULL;
  }
  if( max && !OGRParseXMLDateTime(max, &sMax))
  {
    CPLError(CE_Failure, CPLE_AppDefined,
             "Invalid max: %s",
             max);
    return NULL;
  }
  return (OGRFieldDomainShadow*) OGR_RangeFldDomain_Create( name,
                                                            description,
                                                            OFTDateTime,
                                                            OFSTNone,
                                                            min ? &sMin : NULL,
                                                            minIsInclusive,
                                                            max ? &sMax : NULL,
                                                            maxIsInclusive );
}
%}

%clear const char* name;

%newobject CreateGlobFieldDomain;
%apply Pointer NONNULL {const char* name};
%apply Pointer NONNULL {const char* glob};
%inline %{
static
OGRFieldDomainShadow* CreateGlobFieldDomain( const char *name,
                                             const char* description,
                                             OGRFieldType type,
                                             OGRFieldSubType subtype,
                                             const char* glob ) {
  return (OGRFieldDomainShadow*) OGR_GlobFldDomain_Create( name,
                                                           description,
                                                           type,
                                                           subtype,
                                                           glob );
}
%}
%clear const char* name;
%clear const char* glob;

/************************************************************************/
/*                      OGRGeomCoordinatePrecision                      */
/************************************************************************/

%rename (GeomCoordinatePrecision) OGRGeomCoordinatePrecisionShadow;

class OGRGeomCoordinatePrecisionShadow {
  OGRGeomCoordinatePrecisionShadow();
public:
%extend {

  ~OGRGeomCoordinatePrecisionShadow() {
    OGRGeomCoordinatePrecisionDestroy(self);
  }

  void Set(double xyResolution, double zResolution, double mResolution) {
      OGRGeomCoordinatePrecisionSet(self, xyResolution, zResolution, mResolution);
  }

  %apply Pointer NONNULL {OSRSpatialReferenceShadow* srs};
  void SetFromMeter(OSRSpatialReferenceShadow* srs, double xyMeterResolution, double zMeterResolution, double mResolution) {
      OGRGeomCoordinatePrecisionSetFromMeter(self, srs, xyMeterResolution, zMeterResolution, mResolution);
  }
  %clear OSRSpatialReferenceShadow* srs;

  double GetXYResolution() {
    return OGRGeomCoordinatePrecisionGetXYResolution(self);
  }

  double GetZResolution() {
    return OGRGeomCoordinatePrecisionGetZResolution(self);
  }

  double GetMResolution() {
    return OGRGeomCoordinatePrecisionGetMResolution(self);
  }

%apply (char **CSL) {(char **)};
  char **GetFormats() {
    return OGRGeomCoordinatePrecisionGetFormats(self);
  }
%clear char **;

%apply (char **dict) {char **};
%apply Pointer NONNULL {const char* formatName};
  char ** GetFormatSpecificOptions(const char* formatName) {
    return OGRGeomCoordinatePrecisionGetFormatSpecificOptions(self, formatName);
  }
%clear char **;
%clear const char* formatName;

%apply Pointer NONNULL {const char* formatName};
%apply (char **dict) { char ** formatSpecificOptions };
  void SetFormatSpecificOptions(const char* formatName, char **formatSpecificOptions) {
    OGRGeomCoordinatePrecisionSetFormatSpecificOptions(self, formatName, formatSpecificOptions);
  }
%clear const char* formatName;
%clear char **formatSpecificOptions;

} /* %extend */

}; /* class OGRGeomCoordinatePrecisionShadow */

%newobject CreateGeomCoordinatePrecision;
%inline %{
static
OGRGeomCoordinatePrecisionShadow* CreateGeomCoordinatePrecision() {
  return OGRGeomCoordinatePrecisionCreate();
}
%}

/************************************************************************/
/*                        Other misc functions.                         */
/************************************************************************/

#ifndef FROM_GDAL_I

%{
char const *OGRDriverShadow_get_name( OGRDriverShadow *h ) {
  return OGR_Dr_GetName( h );
}

char const *OGRDataSourceShadow_get_name( OGRDataSourceShadow *h ) {
  return OGR_DS_GetName( h );
}

char const *OGRDriverShadow_name_get( OGRDriverShadow *h ) {
  return OGR_Dr_GetName( h );
}

char const *OGRDataSourceShadow_name_get( OGRDataSourceShadow *h ) {
  return OGR_DS_GetName( h );
}
%}

int OGRGetDriverCount();

#endif /* FROM_GDAL_I */

int OGRGetOpenDSCount();

OGRErr OGRSetGenerate_DB2_V72_BYTE_ORDER(int bGenerate_DB2_V72_BYTE_ORDER);

void OGRRegisterAll();

%rename (GeometryTypeToName) OGRGeometryTypeToName;
const char *OGRGeometryTypeToName( OGRwkbGeometryType eType );

%rename (GetFieldTypeName) OGR_GetFieldTypeName;
const char * OGR_GetFieldTypeName(OGRFieldType type);

%rename (GetFieldSubTypeName) OGR_GetFieldSubTypeName;
const char * OGR_GetFieldSubTypeName(OGRFieldSubType type);

%rename (GT_Flatten) OGR_GT_Flatten;
OGRwkbGeometryType OGR_GT_Flatten( OGRwkbGeometryType eType );

%rename (GT_SetZ) OGR_GT_SetZ;
OGRwkbGeometryType OGR_GT_SetZ( OGRwkbGeometryType eType );

%rename (GT_SetM) OGR_GT_SetM;
OGRwkbGeometryType OGR_GT_SetM( OGRwkbGeometryType eType );

%inline  %{
OGRwkbGeometryType GT_SetModifier( OGRwkbGeometryType eType, int bSetZ, int bSetM = FALSE)
{
    return OGR_GT_SetModifier(eType, bSetZ, bSetM);
}
%}

%rename (GT_HasZ) OGR_GT_HasZ;
int                OGR_GT_HasZ( OGRwkbGeometryType eType );

%rename (GT_HasM) OGR_GT_HasM;
int                OGR_GT_HasM( OGRwkbGeometryType eType );

%rename (GT_IsSubClassOf) OGR_GT_IsSubClassOf;
int                OGR_GT_IsSubClassOf( OGRwkbGeometryType eType,
                                                OGRwkbGeometryType eSuperType );

%rename (GT_IsCurve) OGR_GT_IsCurve;
int                OGR_GT_IsCurve( OGRwkbGeometryType );

%rename (GT_IsSurface) OGR_GT_IsSurface;
int                OGR_GT_IsSurface( OGRwkbGeometryType );

%rename (GT_IsNonLinear) OGR_GT_IsNonLinear;
int                OGR_GT_IsNonLinear( OGRwkbGeometryType );

%rename (GT_GetCollection) OGR_GT_GetCollection;
OGRwkbGeometryType OGR_GT_GetCollection( OGRwkbGeometryType eType );

%rename (GT_GetSingle) OGR_GT_GetSingle;
OGRwkbGeometryType OGR_GT_GetSingle( OGRwkbGeometryType eType );

%rename (GT_GetCurve) OGR_GT_GetCurve;
OGRwkbGeometryType OGR_GT_GetCurve( OGRwkbGeometryType eType );

%rename (GT_GetLinear) OGR_GT_GetLinear;
OGRwkbGeometryType OGR_GT_GetLinear( OGRwkbGeometryType eType );

%rename (SetNonLinearGeometriesEnabledFlag) OGRSetNonLinearGeometriesEnabledFlag;
void OGRSetNonLinearGeometriesEnabledFlag( int bFlag );

%rename (GetNonLinearGeometriesEnabledFlag) OGRGetNonLinearGeometriesEnabledFlag;
int OGRGetNonLinearGeometriesEnabledFlag(void);

%inline %{
  OGRDataSourceShadow* GetOpenDS(int ds_number) {
    OGRDataSourceShadow* layer = (OGRDataSourceShadow*) OGRGetOpenDS(ds_number);
    return layer;
  }
%}

#if !(defined(FROM_GDAL_I) && (defined(SWIGJAVA) || defined(SWIGPYTHON)))

#ifdef SWIGPYTHON
%thread;
#endif
%newobject Open;
#ifndef SWIGJAVA
%feature( "kwargs" ) Open;
#endif
%inline %{
  OGRDataSourceShadow* Open( const char *utf8_path, int update =0 ) {
    CPLErrorReset();
    int nOpenFlags = GDAL_OF_VECTOR;
    if( update )
      nOpenFlags |= GDAL_OF_UPDATE;
#ifdef SWIGPYTHON
    if( GetUseExceptions() )
      nOpenFlags |= GDAL_OF_VERBOSE_ERROR;
#endif
    OGRDataSourceShadow* ds = (OGRDataSourceShadow*)GDALOpenEx( utf8_path, nOpenFlags, NULL,
                                      NULL, NULL );
#ifndef SWIGPYTHON
    if( CPLGetLastErrorType() == CE_Failure && ds != NULL )
    {
        CPLDebug( "SWIG",
		  "OGROpen() succeeded, but an error is posted, so we destroy"
		  " the datasource and fail at swig level." );
        OGRReleaseDataSource(ds);
        ds = NULL;
    }
#endif
    return ds;
  }
%}
#ifdef SWIGPYTHON
%nothread;
#endif

#ifdef SWIGPYTHON
%thread;
#endif
%newobject OpenShared;
#ifndef SWIGJAVA
%feature( "kwargs" ) OpenShared;
#endif
%inline %{
  OGRDataSourceShadow* OpenShared( const char *utf8_path, int update =0 ) {
    CPLErrorReset();
    int nOpenFlags = GDAL_OF_VECTOR | GDAL_OF_SHARED;
    if( update )
      nOpenFlags |= GDAL_OF_UPDATE;
#ifdef SWIGPYTHON
    if( GetUseExceptions() )
      nOpenFlags |= GDAL_OF_VERBOSE_ERROR;
#endif
    OGRDataSourceShadow* ds = (OGRDataSourceShadow*)GDALOpenEx( utf8_path, nOpenFlags, NULL,
                                      NULL, NULL );
#ifndef SWIGPYTHON
    if( CPLGetLastErrorType() == CE_Failure && ds != NULL )
    {
        OGRReleaseDataSource(ds);
        ds = NULL;
    }
#endif
    return ds;
  }
%}
#ifdef SWIGPYTHON
%nothread;
#endif

#endif /* !(defined(FROM_GDAL_I) && (defined(SWIGJAVA) || defined(SWIGPYTHON))) */

#ifndef FROM_GDAL_I

%inline %{
static
OGRDriverShadow* GetDriverByName( char const *name ) {
  return (OGRDriverShadow*) OGRGetDriverByName( name );
}

static
OGRDriverShadow* GetDriver(int driver_number) {
  return (OGRDriverShadow*) OGRGetDriver(driver_number);
}
%}

#if defined(SWIGPYTHON) || defined(SWIGJAVA)
/* FIXME: other bindings should also use those typemaps to avoid memory leaks */
%apply (char **options) {char ** papszArgv};
%apply (char **CSL) {(char **)};
#else
%apply (char **options) {char **};
#endif

/* Interface method added for GDAL 1.7.0 */
#ifdef SWIGJAVA
%inline %{
  static
  char **GeneralCmdLineProcessor( char **papszArgv, int nOptions = 0 ) {
    int nResArgCount;

    /* We must add a 'dummy' element in front of the real argument list */
    /* as Java doesn't include the binary name as the first */
    /* argument, as C does... */
    char** papszArgvModBefore = CSLInsertString(CSLDuplicate(papszArgv), 0, "dummy");
    char** papszArgvModAfter = papszArgvModBefore;

    bool bReloadDrivers = ( CSLFindString(papszArgv, "GDAL_SKIP") >= 0 ||
                            CSLFindString(papszArgv, "OGR_SKIP") >= 0 );

    nResArgCount =
      GDALGeneralCmdLineProcessor( CSLCount(papszArgvModBefore), &papszArgvModAfter, GDAL_OF_VECTOR | nOptions );

    if( bReloadDrivers )
    {
        GDALAllRegister();
    }

    CSLDestroy(papszArgvModBefore);

    if( nResArgCount <= 0 )
    {
        return NULL;
    }
    else
    {
        /* Now, remove the first dummy element */
        char** papszRet = CSLDuplicate(papszArgvModAfter + 1);
        CSLDestroy(papszArgvModAfter);
        return papszRet;
    }
  }
%}
#else
%inline %{
  char **GeneralCmdLineProcessor( char **papszArgv, int nOptions = 0 ) {
    int nResArgCount;

    if( papszArgv == NULL )
        return NULL;

    bool bReloadDrivers = ( CSLFindString(papszArgv, "GDAL_SKIP") >= 0 ||
                            CSLFindString(papszArgv, "OGR_SKIP") >= 0 );

    nResArgCount =
      GDALGeneralCmdLineProcessor( CSLCount(papszArgv), &papszArgv, GDAL_OF_VECTOR | nOptions );

    if( bReloadDrivers )
    {
        GDALAllRegister();
    }

    if( nResArgCount <= 0 )
        return NULL;
    else
        return papszArgv;
  }
%}
#endif
%clear char **;

#endif /* FROM_GDAL_I */

#ifdef SWIGJAVA
class FeatureNative {
  FeatureNative();
  ~FeatureNative();
};

class GeometryNative {
  GeometryNative();
  ~GeometryNative();
};
#endif


/************************************************************************/
/*                            TermProgress()                            */
/************************************************************************/

#ifndef FROM_GDAL_I

#if !defined(SWIGCSHARP) && !defined(SWIGJAVA)
%rename (TermProgress_nocb) GDALTermProgress_nocb;
%feature( "kwargs" ) GDALTermProgress_nocb;
%inline %{
static
int GDALTermProgress_nocb( double dfProgress, const char * pszMessage=NULL, void *pData=NULL ) {
  return GDALTermProgress( dfProgress, pszMessage, pData);
}
%}

%rename (TermProgress) GDALTermProgress;
%callback("%s");
int GDALTermProgress( double, const char *, void * );
%nocallback;
#endif

#endif /* FROM_GDAL_I */

//************************************************************************
//
// Language specific extensions
//
//************************************************************************

#ifdef SWIGCSHARP
%include "ogr_csharp_extend.i"
#endif

#ifdef SWIGJAVA
%include "ogr_java_extend.i"
#endif


#ifdef SWIGPYTHON
%thread;
#endif
