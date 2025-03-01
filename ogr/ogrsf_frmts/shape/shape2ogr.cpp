/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements translation of Shapefile shapes into OGR
 *           representation.
 * Author:   Frank Warmerdam, warmerda@home.com
 *
 ******************************************************************************
 * Copyright (c) 1999,  Les Technologies SoftMap Inc.
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "ogrshape.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogrpgeogeometry.h"
#include "ogrshape.h"
#include "shapefil.h"

/************************************************************************/
/*                        RingStartEnd                                  */
/*        Set first and last vertex for given ring.                     */
/************************************************************************/
static void RingStartEnd(SHPObject *psShape, int ring, int *start, int *end)
{
    if (psShape->panPartStart == nullptr)
    {
        *start = 0;
        *end = psShape->nVertices - 1;
    }
    else
    {
        *start = psShape->panPartStart[ring];

        if (ring == psShape->nParts - 1)
            *end = psShape->nVertices - 1;
        else
            *end = psShape->panPartStart[ring + 1] - 1;
    }
}

/************************************************************************/
/*                        CreateLinearRing                              */
/************************************************************************/
static OGRLinearRing *CreateLinearRing(SHPObject *psShape, int ring, bool bHasZ,
                                       bool bHasM)
{
    int nRingStart = 0;
    int nRingEnd = 0;
    RingStartEnd(psShape, ring, &nRingStart, &nRingEnd);

    OGRLinearRing *const poRing = new OGRLinearRing();
    if (!(nRingEnd >= nRingStart))
        return poRing;

    const int nRingPoints = nRingEnd - nRingStart + 1;

    if (bHasZ && bHasM)
        poRing->setPoints(
            nRingPoints, psShape->padfX + nRingStart,
            psShape->padfY + nRingStart, psShape->padfZ + nRingStart,
            psShape->padfM ? psShape->padfM + nRingStart : nullptr);
    else if (bHasM)
        poRing->setPointsM(nRingPoints, psShape->padfX + nRingStart,
                           psShape->padfY + nRingStart,
                           psShape->padfM ? psShape->padfM + nRingStart
                                          : nullptr);
    else
        poRing->setPoints(nRingPoints, psShape->padfX + nRingStart,
                          psShape->padfY + nRingStart);

    return poRing;
}

/************************************************************************/
/*                          SHPReadOGRObject()                          */
/*                                                                      */
/*      Read an item in a shapefile, and translate to OGR geometry      */
/*      representation.                                                 */
/************************************************************************/

OGRGeometry *SHPReadOGRObject(SHPHandle hSHP, int iShape, SHPObject *psShape,
                              bool &bHasWarnedWrongWindingOrder)
{
#if DEBUG_VERBOSE
    CPLDebug("Shape", "SHPReadOGRObject( iShape=%d )", iShape);
#endif

    if (psShape == nullptr)
        psShape = SHPReadObject(hSHP, iShape);

    if (psShape == nullptr)
    {
        return nullptr;
    }

    OGRGeometry *poOGR = nullptr;

    /* -------------------------------------------------------------------- */
    /*      Point.                                                          */
    /* -------------------------------------------------------------------- */
    if (psShape->nSHPType == SHPT_POINT)
    {
        poOGR = new OGRPoint(psShape->padfX[0], psShape->padfY[0]);
    }
    else if (psShape->nSHPType == SHPT_POINTZ)
    {
        if (psShape->bMeasureIsUsed)
        {
            poOGR = new OGRPoint(psShape->padfX[0], psShape->padfY[0],
                                 psShape->padfZ[0], psShape->padfM[0]);
        }
        else
        {
            poOGR = new OGRPoint(psShape->padfX[0], psShape->padfY[0],
                                 psShape->padfZ[0]);
        }
    }
    else if (psShape->nSHPType == SHPT_POINTM)
    {
        poOGR = new OGRPoint(psShape->padfX[0], psShape->padfY[0], 0.0,
                             psShape->padfM[0]);
        poOGR->set3D(FALSE);
    }
    /* -------------------------------------------------------------------- */
    /*      Multipoint.                                                     */
    /* -------------------------------------------------------------------- */
    else if (psShape->nSHPType == SHPT_MULTIPOINT ||
             psShape->nSHPType == SHPT_MULTIPOINTM ||
             psShape->nSHPType == SHPT_MULTIPOINTZ)
    {
        if (psShape->nVertices == 0)
        {
            poOGR = nullptr;
        }
        else
        {
            OGRMultiPoint *poOGRMPoint = new OGRMultiPoint();

            for (int i = 0; i < psShape->nVertices; i++)
            {
                OGRPoint *poPoint = nullptr;

                if (psShape->nSHPType == SHPT_MULTIPOINTZ)
                {
                    if (psShape->padfM)
                    {
                        poPoint =
                            new OGRPoint(psShape->padfX[i], psShape->padfY[i],
                                         psShape->padfZ[i], psShape->padfM[i]);
                    }
                    else
                    {
                        poPoint =
                            new OGRPoint(psShape->padfX[i], psShape->padfY[i],
                                         psShape->padfZ[i]);
                    }
                }
                else if (psShape->nSHPType == SHPT_MULTIPOINTM &&
                         psShape->padfM)
                {
                    poPoint = new OGRPoint(psShape->padfX[i], psShape->padfY[i],
                                           0.0, psShape->padfM[i]);
                    poPoint->set3D(FALSE);
                }
                else
                {
                    poPoint =
                        new OGRPoint(psShape->padfX[i], psShape->padfY[i]);
                }

                poOGRMPoint->addGeometry(poPoint);

                delete poPoint;
            }

            poOGR = poOGRMPoint;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Arc (LineString)                                                */
    /*                                                                      */
    /*      Ignoring parts though they can apply to arcs as well.           */
    /* -------------------------------------------------------------------- */
    else if (psShape->nSHPType == SHPT_ARC || psShape->nSHPType == SHPT_ARCM ||
             psShape->nSHPType == SHPT_ARCZ)
    {
        if (psShape->nParts == 0)
        {
            poOGR = nullptr;
        }
        else if (psShape->nParts == 1)
        {
            OGRLineString *poOGRLine = new OGRLineString();
            poOGR = poOGRLine;

            if (psShape->nSHPType == SHPT_ARCZ)
                poOGRLine->setPoints(psShape->nVertices, psShape->padfX,
                                     psShape->padfY, psShape->padfZ,
                                     psShape->padfM);
            else if (psShape->nSHPType == SHPT_ARCM)
                poOGRLine->setPointsM(psShape->nVertices, psShape->padfX,
                                      psShape->padfY, psShape->padfM);
            else
                poOGRLine->setPoints(psShape->nVertices, psShape->padfX,
                                     psShape->padfY);
        }
        else
        {
            OGRMultiLineString *poOGRMulti = new OGRMultiLineString();
            poOGR = poOGRMulti;

            for (int iRing = 0; iRing < psShape->nParts; iRing++)
            {
                int nRingPoints = 0;
                int nRingStart = 0;

                OGRLineString *poLine = new OGRLineString();

                if (psShape->panPartStart == nullptr)
                {
                    nRingPoints = psShape->nVertices;
                    nRingStart = 0;
                }
                else
                {
                    if (iRing == psShape->nParts - 1)
                        nRingPoints =
                            psShape->nVertices - psShape->panPartStart[iRing];
                    else
                        nRingPoints = psShape->panPartStart[iRing + 1] -
                                      psShape->panPartStart[iRing];
                    nRingStart = psShape->panPartStart[iRing];
                }

                if (psShape->nSHPType == SHPT_ARCZ)
                    poLine->setPoints(
                        nRingPoints, psShape->padfX + nRingStart,
                        psShape->padfY + nRingStart,
                        psShape->padfZ + nRingStart,
                        psShape->padfM ? psShape->padfM + nRingStart : nullptr);
                else if (psShape->nSHPType == SHPT_ARCM &&
                         psShape->padfM != nullptr)
                    poLine->setPointsM(nRingPoints, psShape->padfX + nRingStart,
                                       psShape->padfY + nRingStart,
                                       psShape->padfM + nRingStart);
                else
                    poLine->setPoints(nRingPoints, psShape->padfX + nRingStart,
                                      psShape->padfY + nRingStart);

                poOGRMulti->addGeometryDirectly(poLine);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Polygon                                                         */
    /*                                                                      */
    /* As for now Z coordinate is not handled correctly                     */
    /* -------------------------------------------------------------------- */
    else if (psShape->nSHPType == SHPT_POLYGON ||
             psShape->nSHPType == SHPT_POLYGONM ||
             psShape->nSHPType == SHPT_POLYGONZ)
    {
        const bool bHasZ = psShape->nSHPType == SHPT_POLYGONZ;
        const bool bHasM = bHasZ || psShape->nSHPType == SHPT_POLYGONM;

#if DEBUG_VERBOSE
        CPLDebug("Shape", "Shape type: polygon with nParts=%d",
                 psShape->nParts);
#endif

        if (psShape->nParts == 0)
        {
            poOGR = nullptr;
        }
        else if (psShape->nParts == 1)
        {
            // Surely outer ring.
            OGRPolygon *poOGRPoly = new OGRPolygon();
            poOGR = poOGRPoly;

            OGRLinearRing *poRing = CreateLinearRing(psShape, 0, bHasZ, bHasM);
            poOGRPoly->addRingDirectly(poRing);
        }
        else
        {
            OGRPolygon **tabPolygons = new OGRPolygon *[psShape->nParts];
            tabPolygons[0] = new OGRPolygon();
            auto poExteriorRing = CreateLinearRing(psShape, 0, bHasZ, bHasM);
            tabPolygons[0]->addRingDirectly(poExteriorRing);
            for (int iRing = 1; iRing < psShape->nParts; iRing++)
            {
                tabPolygons[iRing] = new OGRPolygon();
                tabPolygons[iRing]->addRingDirectly(
                    CreateLinearRing(psShape, iRing, bHasZ, bHasM));
            }

            // Tries to detect bad geometries where a multi-part multipolygon is
            // written as a single-part multipolygon with its parts as inner
            // rings, like done by QGIS <= 3.28.11 with GDAL >= 3.7
            // Cf https://github.com/qgis/QGIS/issues/54537
            bool bUseSlowMethod = false;
            if (!bHasZ && !bHasM)
            {
                bool bFoundCW = false;
                for (int iRing = 1; iRing < psShape->nParts; iRing++)
                {
                    if (tabPolygons[iRing]->getExteriorRing()->isClockwise())
                    {
                        bFoundCW = true;
                        break;
                    }
                }
                if (!bFoundCW)
                {
                    // Only inner rings
                    OGREnvelope sFirstEnvelope;
                    OGREnvelope sCurEnvelope;
                    poExteriorRing->getEnvelope(&sFirstEnvelope);
                    for (int iRing = 1; iRing < psShape->nParts; iRing++)
                    {
                        tabPolygons[iRing]->getEnvelope(&sCurEnvelope);
                        if (!sFirstEnvelope.Intersects(sCurEnvelope))
                        {
                            // If the envelopes of the rings don't intersect,
                            // then it is clearly a multi-part polygon
                            bUseSlowMethod = true;
                            break;
                        }
                        else
                        {
                            // Otherwise take 4 points at each extremity of
                            // the inner rings and check if there are in the
                            // outer ring. If none are within it, then it is
                            // very likely a outer ring (or an invalid ring
                            // which is neither a outer nor a inner ring)
                            auto poRing = tabPolygons[iRing]->getExteriorRing();
                            const auto nNumPoints = poRing->getNumPoints();
                            OGRPoint p;
                            OGRPoint leftPoint(
                                std::numeric_limits<double>::infinity(), 0);
                            OGRPoint rightPoint(
                                -std::numeric_limits<double>::infinity(), 0);
                            OGRPoint bottomPoint(
                                0, std::numeric_limits<double>::infinity());
                            OGRPoint topPoint(
                                0, -std::numeric_limits<double>::infinity());
                            for (int iPoint = 0; iPoint < nNumPoints - 1;
                                 ++iPoint)
                            {
                                poRing->getPoint(iPoint, &p);
                                if (p.getX() < leftPoint.getX() ||
                                    (p.getX() == leftPoint.getX() &&
                                     p.getY() < leftPoint.getY()))
                                {
                                    leftPoint = p;
                                }
                                if (p.getX() > rightPoint.getX() ||
                                    (p.getX() == rightPoint.getX() &&
                                     p.getY() > rightPoint.getY()))
                                {
                                    rightPoint = p;
                                }
                                if (p.getY() < bottomPoint.getY() ||
                                    (p.getY() == bottomPoint.getY() &&
                                     p.getX() > bottomPoint.getX()))
                                {
                                    bottomPoint = p;
                                }
                                if (p.getY() > topPoint.getY() ||
                                    (p.getY() == topPoint.getY() &&
                                     p.getX() < topPoint.getX()))
                                {
                                    topPoint = p;
                                }
                            }
                            if (!poExteriorRing->isPointInRing(&leftPoint) &&
                                !poExteriorRing->isPointInRing(&rightPoint) &&
                                !poExteriorRing->isPointInRing(&bottomPoint) &&
                                !poExteriorRing->isPointInRing(&topPoint))
                            {
                                bUseSlowMethod = true;
                                break;
                            }
                        }
                    }
                    if (bUseSlowMethod && !bHasWarnedWrongWindingOrder)
                    {
                        bHasWarnedWrongWindingOrder = true;
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "%s contains polygon(s) with rings with "
                                 "invalid winding order. Autocorrecting them, "
                                 "but that shapefile should be corrected using "
                                 "ogr2ogr for example.",
                                 VSI_SHP_GetFilename(hSHP->fpSHP));
                    }
                }
            }

            int isValidGeometry = FALSE;
            const char *papszOptions[] = {
                bUseSlowMethod ? "METHOD=DEFAULT" : "METHOD=ONLY_CCW", nullptr};
            OGRGeometry **tabGeom =
                reinterpret_cast<OGRGeometry **>(tabPolygons);
            poOGR = OGRGeometryFactory::organizePolygons(
                tabGeom, psShape->nParts, &isValidGeometry, papszOptions);

            if (!isValidGeometry)
            {
                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "Geometry of polygon of fid %d cannot be translated to "
                    "Simple Geometry. "
                    "All polygons will be contained in a multipolygon.",
                    iShape);
            }

            delete[] tabPolygons;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      MultiPatch                                                      */
    /* -------------------------------------------------------------------- */
    else if (psShape->nSHPType == SHPT_MULTIPATCH)
    {
        poOGR = OGRCreateFromMultiPatch(
            psShape->nParts, psShape->panPartStart, psShape->panPartType,
            psShape->nVertices, psShape->padfX, psShape->padfY, psShape->padfZ);
    }

    /* -------------------------------------------------------------------- */
    /*      Otherwise for now we just ignore the object.                    */
    /* -------------------------------------------------------------------- */
    else
    {
        if (psShape->nSHPType != SHPT_NULL)
        {
            CPLDebug("OGR", "Unsupported shape type in SHPReadOGRObject()");
        }

        // Nothing returned.
    }

    /* -------------------------------------------------------------------- */
    /*      Cleanup shape, and set feature id.                              */
    /* -------------------------------------------------------------------- */
    SHPDestroyObject(psShape);

    return poOGR;
}

/************************************************************************/
/*                      CheckNonFiniteCoordinates()                     */
/************************************************************************/

static bool CheckNonFiniteCoordinates(const double *v, size_t vsize)
{
    static bool bAllowNonFiniteCoordinates = CPLTestBool(
        CPLGetConfigOption("OGR_SHAPE_ALLOW_NON_FINITE_COORDINATES", "NO"));
    // Do not document this. Only for edge case testing
    if (bAllowNonFiniteCoordinates)
    {
        return true;
    }
    for (size_t i = 0; i < vsize; ++i)
    {
        if (!std::isfinite(v[i]))
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Coordinates with non-finite values are not allowed");
            return false;
        }
    }
    return true;
}

static bool CheckNonFiniteCoordinates(const std::vector<double> &v)
{
    return CheckNonFiniteCoordinates(v.data(), v.size());
}

/************************************************************************/
/*                         SHPWriteOGRObject()                          */
/************************************************************************/
static OGRErr SHPWriteOGRObject(SHPHandle hSHP, int iShape,
                                const OGRGeometry *poGeom, bool bRewind,
                                OGRwkbGeometryType eLayerGeomType)

{
    /* ==================================================================== */
    /*      Write "shape" with no geometry or with empty geometry           */
    /* ==================================================================== */
    if (poGeom == nullptr || poGeom->IsEmpty())
    {
        SHPObject *psShape =
            SHPCreateObject(SHPT_NULL, -1, 0, nullptr, nullptr, 0, nullptr,
                            nullptr, nullptr, nullptr);
        const int nReturnedShapeID = SHPWriteObject(hSHP, iShape, psShape);
        SHPDestroyObject(psShape);
        if (nReturnedShapeID == -1)
        {
            // Assuming error is reported by SHPWriteObject().
            return OGRERR_FAILURE;
        }
    }

    /* ==================================================================== */
    /*      Write point geometry.                                           */
    /* ==================================================================== */
    else if (hSHP->nShapeType == SHPT_POINT ||
             hSHP->nShapeType == SHPT_POINTM || hSHP->nShapeType == SHPT_POINTZ)
    {
        if (wkbFlatten(poGeom->getGeometryType()) != wkbPoint)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Attempt to write non-point (%s) geometry to"
                     " point shapefile.",
                     poGeom->getGeometryName());

            return OGRERR_UNSUPPORTED_GEOMETRY_TYPE;
        }

        const OGRPoint *poPoint = poGeom->toPoint();
        const double dfX = poPoint->getX();
        const double dfY = poPoint->getY();
        const double dfZ = poPoint->getZ();
        double dfM = -std::numeric_limits<double>::max();
        double *pdfM = nullptr;
        if (wkbHasM(eLayerGeomType) && (hSHP->nShapeType == SHPT_POINTM ||
                                        hSHP->nShapeType == SHPT_POINTZ))
        {
            if (poGeom->IsMeasured())
                dfM = poPoint->getM();
            pdfM = &dfM;
        }
        if ((!std::isfinite(dfX) || !std::isfinite(dfY) ||
             !std::isfinite(dfZ) || (pdfM && !std::isfinite(*pdfM))) &&
            !CPLTestBool(CPLGetConfigOption(
                "OGR_SHAPE_ALLOW_NON_FINITE_COORDINATES", "NO")))
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Coordinates with non-finite values are not allowed");
            return OGRERR_FAILURE;
        }
        SHPObject *psShape =
            SHPCreateObject(hSHP->nShapeType, -1, 0, nullptr, nullptr, 1, &dfX,
                            &dfY, &dfZ, pdfM);
        const int nReturnedShapeID = SHPWriteObject(hSHP, iShape, psShape);
        SHPDestroyObject(psShape);
        if (nReturnedShapeID == -1)
            return OGRERR_FAILURE;
    }
    /* ==================================================================== */
    /*      MultiPoint.                                                     */
    /* ==================================================================== */
    else if (hSHP->nShapeType == SHPT_MULTIPOINT ||
             hSHP->nShapeType == SHPT_MULTIPOINTM ||
             hSHP->nShapeType == SHPT_MULTIPOINTZ)
    {
        if (wkbFlatten(poGeom->getGeometryType()) != wkbMultiPoint)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Attempt to write non-multipoint (%s) geometry to "
                     "multipoint shapefile.",
                     poGeom->getGeometryName());

            return OGRERR_UNSUPPORTED_GEOMETRY_TYPE;
        }

        const OGRMultiPoint *poMP = poGeom->toMultiPoint();
        const int nNumGeometries = poMP->getNumGeometries();
        const bool bHasZ = (hSHP->nShapeType == SHPT_MULTIPOINTM ||
                            hSHP->nShapeType == SHPT_MULTIPOINTZ);
        const bool bHasM = wkbHasM(eLayerGeomType) && bHasZ;
        const bool bIsGeomMeasured = CPL_TO_BOOL(poGeom->IsMeasured());

        std::vector<double> adfX;
        std::vector<double> adfY;
        std::vector<double> adfZ;
        std::vector<double> adfM;
        try
        {
            adfX.reserve(nNumGeometries);
            adfY.reserve(nNumGeometries);
            if (bHasZ)
                adfZ.reserve(nNumGeometries);
            if (bHasM)
                adfM.reserve(nNumGeometries);
        }
        catch (const std::exception &e)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "%s", e.what());
            return OGRERR_FAILURE;
        }

        for (const OGRPoint *poPoint : *poMP)
        {
            // Ignore POINT EMPTY.
            if (!poPoint->IsEmpty())
            {
                adfX.push_back(poPoint->getX());
                adfY.push_back(poPoint->getY());
                if (bHasZ)
                    adfZ.push_back(poPoint->getZ());
                if (bHasM)
                {
                    if (bIsGeomMeasured)
                        adfM.push_back(poPoint->getM());
                    else
                        adfM.push_back(-std::numeric_limits<double>::max());
                }
            }
            else
            {
                CPLDebug("OGR",
                         "Ignored POINT EMPTY inside MULTIPOINT in shapefile "
                         "writer.");
            }
        }
        if (!CheckNonFiniteCoordinates(adfX) ||
            !CheckNonFiniteCoordinates(adfY) ||
            !CheckNonFiniteCoordinates(adfZ) ||
            !CheckNonFiniteCoordinates(adfM))
        {
            return OGRERR_FAILURE;
        }

        SHPObject *psShape = SHPCreateObject(
            hSHP->nShapeType, -1, 0, nullptr, nullptr,
            static_cast<int>(adfX.size()), adfX.data(), adfY.data(),
            bHasZ ? adfZ.data() : nullptr, bHasM ? adfM.data() : nullptr);
        const int nReturnedShapeID = SHPWriteObject(hSHP, iShape, psShape);
        SHPDestroyObject(psShape);

        if (nReturnedShapeID == -1)
            return OGRERR_FAILURE;
    }

    /* ==================================================================== */
    /*      Arcs                                                            */
    /* ==================================================================== */
    else if (hSHP->nShapeType == SHPT_ARC || hSHP->nShapeType == SHPT_ARCM ||
             hSHP->nShapeType == SHPT_ARCZ)
    {
        std::unique_ptr<OGRGeometry> poGeomToDelete;  // keep in that scope
        const OGRMultiLineString *poML = nullptr;
        OGRMultiLineString oMLFromLineString;
        const auto eFlatGeomType = wkbFlatten(poGeom->getGeometryType());
        if (eFlatGeomType == wkbMultiLineString)
        {
            poML = poGeom->toMultiLineString();
        }
        else if (eFlatGeomType == wkbLineString)
        {
            // Borrow the geometry
            oMLFromLineString.addGeometryDirectly(
                const_cast<OGRLineString *>(poGeom->toLineString()));
            poML = &oMLFromLineString;
        }
        else
        {
            poGeomToDelete = std::unique_ptr<OGRGeometry>(
                OGRGeometryFactory::forceToMultiLineString(poGeom->clone()));
            if (wkbFlatten(poGeomToDelete->getGeometryType()) !=
                wkbMultiLineString)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Attempt to write non-linestring (%s) geometry to "
                         "ARC type shapefile.",
                         poGeom->getGeometryName());

                return OGRERR_UNSUPPORTED_GEOMETRY_TYPE;
            }
            poML = poGeomToDelete->toMultiLineString();
        }

        const int nNumGeometries = poML->getNumGeometries();

        int nTotalPoints = 0;
        for (const auto poArc : poML)
        {
            const int nNumPoints = poArc->getNumPoints();
            if (nTotalPoints > std::numeric_limits<int>::max() - nNumPoints)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Too big geometry");
                return OGRERR_FAILURE;
            }
            nTotalPoints += nNumPoints;
        }

        std::vector<int> anRingStart;
        std::vector<double> adfX;
        std::vector<double> adfY;
        std::vector<double> adfZ;
        std::vector<double> adfM;
        const bool bHasZ =
            (hSHP->nShapeType == SHPT_ARCM || hSHP->nShapeType == SHPT_ARCZ);
        const bool bHasM = wkbHasM(eLayerGeomType) && bHasZ;
        const bool bIsGeomMeasured = CPL_TO_BOOL(poGeom->IsMeasured());

        try
        {
            anRingStart.reserve(nNumGeometries);

            adfX.reserve(nTotalPoints);
            adfY.reserve(nTotalPoints);
            if (bHasZ)
            {
                adfZ.reserve(nTotalPoints);
            }
            if (bHasM)
            {
                adfM.reserve(nTotalPoints);
            }
        }
        catch (const std::exception &e)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "%s", e.what());
            // Give back the borrowed line string
            if (eFlatGeomType == wkbLineString)
                oMLFromLineString.removeGeometry(0, /* bDelete=*/false);
            return OGRERR_FAILURE;
        }

        for (const auto poArc : poML)
        {
            const int nNumPoints = poArc->getNumPoints();

            // Ignore LINESTRING EMPTY.
            if (nNumPoints == 0)
            {
                CPLDebug("OGR",
                         "Ignore LINESTRING EMPTY inside MULTILINESTRING in "
                         "shapefile writer.");
                continue;
            }

            anRingStart.push_back(static_cast<int>(adfX.size()));

            for (int iPoint = 0; iPoint < nNumPoints; iPoint++)
            {
                adfX.push_back(poArc->getX(iPoint));
                adfY.push_back(poArc->getY(iPoint));
                if (bHasZ)
                {
                    adfZ.push_back(poArc->getZ(iPoint));
                }
                if (bHasM)
                {
                    if (bIsGeomMeasured)
                        adfM.push_back(poArc->getM(iPoint));
                    else
                        adfM.push_back(-std::numeric_limits<double>::max());
                }
            }
        }

        // Give back the borrowed line string
        if (eFlatGeomType == wkbLineString)
            oMLFromLineString.removeGeometry(0, /* bDelete=*/false);

        if (!CheckNonFiniteCoordinates(adfX) ||
            !CheckNonFiniteCoordinates(adfY) ||
            !CheckNonFiniteCoordinates(adfZ) ||
            !CheckNonFiniteCoordinates(adfM))
        {
            return OGRERR_FAILURE;
        }

        SHPObject *psShape = SHPCreateObject(
            hSHP->nShapeType, iShape, static_cast<int>(anRingStart.size()),
            anRingStart.data(), nullptr, static_cast<int>(adfX.size()),
            adfX.data(), adfY.data(), bHasZ ? adfZ.data() : nullptr,
            bHasM ? adfM.data() : nullptr);
        const int nReturnedShapeID = SHPWriteObject(hSHP, iShape, psShape);
        SHPDestroyObject(psShape);

        if (nReturnedShapeID == -1)
            return OGRERR_FAILURE;
    }

    /* ==================================================================== */
    /*      Polygons/MultiPolygons                                          */
    /* ==================================================================== */
    else if (hSHP->nShapeType == SHPT_POLYGON ||
             hSHP->nShapeType == SHPT_POLYGONM ||
             hSHP->nShapeType == SHPT_POLYGONZ)
    {
        // bool = true means outer ring
        std::vector<std::pair<const OGRLinearRing *, bool>> apoRings;
        const OGRwkbGeometryType eType = wkbFlatten(poGeom->getGeometryType());
        std::unique_ptr<OGRGeometry> poGeomToDelete;

        if (eType == wkbPolygon || eType == wkbTriangle)
        {
            const OGRPolygon *poPoly = poGeom->toPolygon();

            if (poPoly->getExteriorRing() == nullptr ||
                poPoly->getExteriorRing()->IsEmpty())
            {
                CPLDebug("OGR", "Ignore POLYGON EMPTY in shapefile writer.");
            }
            else
            {
                const int nSrcRings = poPoly->getNumInteriorRings() + 1;
                apoRings.reserve(nSrcRings);
                bool bFirstRing = true;
                for (const auto poRing : poPoly)
                {
                    const int nNumPoints = poRing->getNumPoints();

                    // Ignore LINEARRING EMPTY.
                    if (nNumPoints != 0)
                    {
                        apoRings.push_back(std::make_pair(poRing, bFirstRing));
                    }
                    else
                    {
                        CPLDebug("OGR",
                                 "Ignore LINEARRING EMPTY inside POLYGON in "
                                 "shapefile writer.");
                    }
                    bFirstRing = false;
                }
            }
        }
        else if (eType == wkbMultiPolygon || eType == wkbGeometryCollection ||
                 eType == wkbPolyhedralSurface || eType == wkbTIN)
        {
            const OGRGeometryCollection *poGC;
            // for PolyhedralSurface and TIN
            if (eType == wkbPolyhedralSurface || eType == wkbTIN)
            {
                poGeomToDelete =
                    std::unique_ptr<OGRGeometry>(OGRGeometryFactory::forceTo(
                        poGeom->clone(), wkbMultiPolygon, nullptr));
                poGC = poGeomToDelete->toGeometryCollection();
            }

            else
                poGC = poGeom->toGeometryCollection();

            // Shouldn't happen really, but to please x86_64-w64-mingw32-g++ -O2
            // -Wnull-dereference
            if (poGC == nullptr)
                return OGRERR_UNSUPPORTED_GEOMETRY_TYPE;

            for (const auto poSubGeom : poGC)
            {
                if (wkbFlatten(poSubGeom->getGeometryType()) != wkbPolygon)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Attempt to write non-polygon (%s) geometry to "
                             "POLYGON type shapefile.",
                             poSubGeom->getGeometryName());

                    return OGRERR_UNSUPPORTED_GEOMETRY_TYPE;
                }
                const OGRPolygon *poPoly = poSubGeom->toPolygon();

                // Ignore POLYGON EMPTY.
                if (poPoly->getExteriorRing() == nullptr ||
                    poPoly->getExteriorRing()->IsEmpty())
                {
                    CPLDebug("OGR",
                             "Ignore POLYGON EMPTY inside MULTIPOLYGON in "
                             "shapefile writer.");
                    continue;
                }

                const int nNumInteriorRings = poPoly->getNumInteriorRings();
                // to avoid coverity scan warning: "To avoid a quadratic time
                // penalty when using reserve(), always increase the capacity
                /// by a multiple of its current value"
                if (apoRings.size() + nNumInteriorRings + 1 >
                        apoRings.capacity() &&
                    apoRings.size() < std::numeric_limits<size_t>::max() / 2)
                {
                    apoRings.reserve(std::max(
                        2 * apoRings.size(), apoRings.size() + apoRings.size() +
                                                 nNumInteriorRings + 1));
                }
                bool bFirstRing = true;
                for (const auto poRing : poPoly)
                {
                    const int nNumPoints = poRing->getNumPoints();

                    // Ignore LINEARRING EMPTY.
                    if (nNumPoints != 0)
                    {
                        apoRings.push_back(std::make_pair(poRing, bFirstRing));
                    }
                    else
                    {
                        CPLDebug("OGR",
                                 "Ignore LINEARRING EMPTY inside POLYGON in "
                                 "shapefile writer.");
                    }
                    bFirstRing = false;
                }
            }
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Attempt to write non-polygon (%s) geometry to "
                     "POLYGON type shapefile.",
                     poGeom->getGeometryName());

            return OGRERR_UNSUPPORTED_GEOMETRY_TYPE;
        }

        /* --------------------------------------------------------------------
         */
        /*      If we only had emptypolygons or unacceptable geometries */
        /*      write NULL geometry object. */
        /* --------------------------------------------------------------------
         */
        if (apoRings.empty())
        {
            SHPObject *psShape =
                SHPCreateObject(SHPT_NULL, -1, 0, nullptr, nullptr, 0, nullptr,
                                nullptr, nullptr, nullptr);
            const int nReturnedShapeID = SHPWriteObject(hSHP, iShape, psShape);
            SHPDestroyObject(psShape);

            if (nReturnedShapeID == -1)
                return OGRERR_FAILURE;

            return OGRERR_NONE;
        }

        // Count vertices.
        int nVertex = 0;
        for (const auto &ring : apoRings)
            nVertex += ring.first->getNumPoints();

        const bool bHasZ = (hSHP->nShapeType == SHPT_POLYGONM ||
                            hSHP->nShapeType == SHPT_POLYGONZ);
        const bool bHasM = wkbHasM(eLayerGeomType) && bHasZ;
        const bool bIsGeomMeasured = CPL_TO_BOOL(poGeom->IsMeasured());

        std::vector<int> anRingStart;
        std::vector<double> adfX;
        std::vector<double> adfY;
        std::vector<double> adfZ;
        std::vector<double> adfM;
        try
        {
            anRingStart.reserve(apoRings.size());
            adfX.reserve(nVertex);
            adfY.reserve(nVertex);
            if (bHasZ)
                adfZ.reserve(nVertex);
            if (bHasM)
                adfM.reserve(nVertex);
        }
        catch (const std::exception &e)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "%s", e.what());
            return OGRERR_FAILURE;
        }

        // Collect vertices.
        for (const auto &ring : apoRings)
        {
            const auto poRing = ring.first;
            const bool bIsOuterRing = ring.second;
            anRingStart.push_back(static_cast<int>(adfX.size()));

            const int nNumPoints = poRing->getNumPoints();
            // Exterior ring must be clockwise oriented in shapefiles
            const bool bInvertOrder =
                !bRewind && CPL_TO_BOOL(bIsOuterRing ? !poRing->isClockwise()
                                                     : poRing->isClockwise());
            for (int i = 0; i < nNumPoints; i++)
            {
                const int iPoint = bInvertOrder ? nNumPoints - 1 - i : i;
                adfX.push_back(poRing->getX(iPoint));
                adfY.push_back(poRing->getY(iPoint));
                if (bHasZ)
                    adfZ.push_back(poRing->getZ(iPoint));
                if (bHasM)
                {
                    adfM.push_back(bIsGeomMeasured
                                       ? poRing->getM(iPoint)
                                       : -std::numeric_limits<double>::max());
                }
            }
        }
        if (!CheckNonFiniteCoordinates(adfX) ||
            !CheckNonFiniteCoordinates(adfY) ||
            !CheckNonFiniteCoordinates(adfZ) ||
            !CheckNonFiniteCoordinates(adfM))
        {
            return OGRERR_FAILURE;
        }

        SHPObject *psShape = SHPCreateObject(
            hSHP->nShapeType, iShape, static_cast<int>(anRingStart.size()),
            anRingStart.data(), nullptr, static_cast<int>(adfX.size()),
            adfX.data(), adfY.data(), bHasZ ? adfZ.data() : nullptr,
            bHasM ? adfM.data() : nullptr);
        if (bRewind)
            SHPRewindObject(hSHP, psShape);
        const int nReturnedShapeID = SHPWriteObject(hSHP, iShape, psShape);
        SHPDestroyObject(psShape);

        if (nReturnedShapeID == -1)
            return OGRERR_FAILURE;
    }

    /* ==================================================================== */
    /*      Multipatch                                                      */
    /* ==================================================================== */
    else if (hSHP->nShapeType == SHPT_MULTIPATCH)
    {
        int nParts = 0;
        std::vector<int> anPartStart;
        std::vector<int> anPartType;
        int nPoints = 0;
        std::vector<OGRRawPoint> aoPoints;
        std::vector<double> adfZ;
        OGRErr eErr = OGRCreateMultiPatch(poGeom,
                                          FALSE,  // no SHPP_TRIANGLES
                                          nParts, anPartStart, anPartType,
                                          nPoints, aoPoints, adfZ);
        if (eErr != OGRERR_NONE)
            return OGRERR_UNSUPPORTED_GEOMETRY_TYPE;

        std::vector<double> adfX(nPoints);
        std::vector<double> adfY(nPoints);
        for (int i = 0; i < nPoints; ++i)
        {
            adfX[i] = aoPoints[i].x;
            adfY[i] = aoPoints[i].y;
        }

        if (!CheckNonFiniteCoordinates(adfX.data(), nPoints) ||
            !CheckNonFiniteCoordinates(adfY.data(), nPoints) ||
            !CheckNonFiniteCoordinates(adfZ.data(), nPoints))
        {
            return OGRERR_FAILURE;
        }

        SHPObject *psShape =
            SHPCreateObject(hSHP->nShapeType, iShape, nParts,
                            anPartStart.data(), anPartType.data(), nPoints,
                            adfX.data(), adfY.data(), adfZ.data(), nullptr);
        if (bRewind)
            SHPRewindObject(hSHP, psShape);
        const int nReturnedShapeID = SHPWriteObject(hSHP, iShape, psShape);
        SHPDestroyObject(psShape);

        if (nReturnedShapeID == -1)
            return OGRERR_FAILURE;
    }

    else
    {
        return OGRERR_UNSUPPORTED_GEOMETRY_TYPE;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                       SHPReadOGRFeatureDefn()                        */
/************************************************************************/

OGRFeatureDefn *SHPReadOGRFeatureDefn(const char *pszName, SHPHandle hSHP,
                                      DBFHandle hDBF,
                                      const char *pszSHPEncoding,
                                      int bAdjustType)

{
    int nAdjustableFields = 0;
    const int nFieldCount = hDBF ? DBFGetFieldCount(hDBF) : 0;

    OGRFeatureDefn *const poDefn = new OGRFeatureDefn(pszName);
    poDefn->Reference();

    for (int iField = 0; iField < nFieldCount; iField++)
    {
        // On reading we support up to 11 characters
        char szFieldName[XBASE_FLDNAME_LEN_READ + 1] = {};
        int nWidth = 0;
        int nPrecision = 0;
        DBFFieldType eDBFType =
            DBFGetFieldInfo(hDBF, iField, szFieldName, &nWidth, &nPrecision);

        OGRFieldDefn oField("", OFTInteger);
        if (strlen(pszSHPEncoding) > 0)
        {
            char *const pszUTF8Field =
                CPLRecode(szFieldName, pszSHPEncoding, CPL_ENC_UTF8);
            oField.SetName(pszUTF8Field);
            CPLFree(pszUTF8Field);
        }
        else
        {
            oField.SetName(szFieldName);
        }

        oField.SetWidth(nWidth);
        oField.SetPrecision(nPrecision);

        if (eDBFType == FTDate)
        {
            // Shapefile date has following 8-chars long format:
            //
            //     20060101.
            //
            // Split as YYYY/MM/DD, so 2 additional characters are required.
            oField.SetWidth(nWidth + 2);
            oField.SetType(OFTDate);
        }
        else if (eDBFType == FTDouble)
        {
            nAdjustableFields += (nPrecision == 0);
            if (nPrecision == 0 && nWidth < 19)
                oField.SetType(OFTInteger64);
            else
                oField.SetType(OFTReal);
        }
        else if (eDBFType == FTInteger)
            oField.SetType(OFTInteger);
        else if (eDBFType == FTLogical)
        {
            oField.SetType(OFTInteger);
            oField.SetSubType(OFSTBoolean);
        }
        else
            oField.SetType(OFTString);

        poDefn->AddFieldDefn(&oField);
    }

    // Do an optional past if requested and needed to demote Integer64->Integer
    // or Real->Integer64/Integer.
    if (nAdjustableFields && bAdjustType)
    {
        int *panAdjustableField =
            static_cast<int *>(CPLCalloc(sizeof(int), nFieldCount));
        for (int iField = 0; iField < nFieldCount; iField++)
        {
            OGRFieldType eType = poDefn->GetFieldDefn(iField)->GetType();
            if (poDefn->GetFieldDefn(iField)->GetPrecision() == 0 &&
                (eType == OFTInteger64 || eType == OFTReal))
            {
                panAdjustableField[iField] = TRUE;
                poDefn->GetFieldDefn(iField)->SetType(OFTInteger);
            }
        }

        const int nRowCount = DBFGetRecordCount(hDBF);
        for (int iRow = 0; iRow < nRowCount && nAdjustableFields; iRow++)
        {
            for (int iField = 0; iField < nFieldCount; iField++)
            {
                if (panAdjustableField[iField])
                {
                    const char *pszValue =
                        DBFReadStringAttribute(hDBF, iRow, iField);
                    const int nValueLength = static_cast<int>(strlen(pszValue));
                    if (nValueLength >= 10)
                    {
                        int bOverflow = FALSE;
                        const GIntBig nVal =
                            CPLAtoGIntBigEx(pszValue, FALSE, &bOverflow);
                        if (bOverflow)
                        {
                            poDefn->GetFieldDefn(iField)->SetType(OFTReal);
                            panAdjustableField[iField] = FALSE;
                            nAdjustableFields--;
                        }
                        else if (!CPL_INT64_FITS_ON_INT32(nVal))
                        {
                            poDefn->GetFieldDefn(iField)->SetType(OFTInteger64);
                            if (poDefn->GetFieldDefn(iField)->GetWidth() <= 18)
                            {
                                panAdjustableField[iField] = FALSE;
                                nAdjustableFields--;
                            }
                        }
                    }
                }
            }
        }

        CPLFree(panAdjustableField);
    }

    if (hSHP == nullptr)
    {
        poDefn->SetGeomType(wkbNone);
    }
    else
    {
        switch (hSHP->nShapeType)
        {
            case SHPT_POINT:
                poDefn->SetGeomType(wkbPoint);
                break;

            case SHPT_POINTZ:
                poDefn->SetGeomType(wkbPointZM);
                break;

            case SHPT_POINTM:
                poDefn->SetGeomType(wkbPointM);
                break;

            case SHPT_ARC:
                poDefn->SetGeomType(wkbLineString);
                break;

            case SHPT_ARCZ:
                poDefn->SetGeomType(wkbLineStringZM);
                break;

            case SHPT_ARCM:
                poDefn->SetGeomType(wkbLineStringM);
                break;

            case SHPT_MULTIPOINT:
                poDefn->SetGeomType(wkbMultiPoint);
                break;

            case SHPT_MULTIPOINTZ:
                poDefn->SetGeomType(wkbMultiPointZM);
                break;

            case SHPT_MULTIPOINTM:
                poDefn->SetGeomType(wkbMultiPointM);
                break;

            case SHPT_POLYGON:
                poDefn->SetGeomType(wkbPolygon);
                break;

            case SHPT_POLYGONZ:
                poDefn->SetGeomType(wkbPolygonZM);
                break;

            case SHPT_POLYGONM:
                poDefn->SetGeomType(wkbPolygonM);
                break;

            case SHPT_MULTIPATCH:
                poDefn->SetGeomType(wkbUnknown);  // not ideal
                break;
        }
    }

    return poDefn;
}

/************************************************************************/
/*                         SHPReadOGRFeature()                          */
/************************************************************************/

OGRFeature *SHPReadOGRFeature(SHPHandle hSHP, DBFHandle hDBF,
                              OGRFeatureDefn *poDefn, int iShape,
                              SHPObject *psShape, const char *pszSHPEncoding,
                              bool &bHasWarnedWrongWindingOrder)

{
    if (iShape < 0 || (hSHP != nullptr && iShape >= hSHP->nRecords) ||
        (hDBF != nullptr && iShape >= hDBF->nRecords))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to read shape with feature id (%d) out of available"
                 " range.",
                 iShape);
        return nullptr;
    }

    if (hDBF && DBFIsRecordDeleted(hDBF, iShape))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to read shape with feature id (%d), "
                 "but it is marked deleted.",
                 iShape);
        if (psShape != nullptr)
            SHPDestroyObject(psShape);
        return nullptr;
    }

    OGRFeature *poFeature = new OGRFeature(poDefn);

    /* -------------------------------------------------------------------- */
    /*      Fetch geometry from Shapefile to OGRFeature.                    */
    /* -------------------------------------------------------------------- */
    if (hSHP != nullptr)
    {
        if (!poDefn->IsGeometryIgnored())
        {
            OGRGeometry *poGeometry = SHPReadOGRObject(
                hSHP, iShape, psShape, bHasWarnedWrongWindingOrder);

            // Two possibilities are expected here (both are tested by
            // GDAL Autotests):
            //   1. Read valid geometry and assign it directly.
            //   2. Read and assign null geometry if it can not be read
            //      correctly from a shapefile.
            //
            // It is NOT required here to test poGeometry == NULL.

            if (poGeometry)
            {
                // Set/unset flags.
                const OGRwkbGeometryType eMyGeomType =
                    poFeature->GetDefnRef()->GetGeomFieldDefn(0)->GetType();

                if (eMyGeomType != wkbUnknown)
                {
                    OGRwkbGeometryType eGeomInType =
                        poGeometry->getGeometryType();
                    if (wkbHasZ(eMyGeomType) && !wkbHasZ(eGeomInType))
                    {
                        poGeometry->set3D(TRUE);
                    }
                    else if (!wkbHasZ(eMyGeomType) && wkbHasZ(eGeomInType))
                    {
                        poGeometry->set3D(FALSE);
                    }
                    if (wkbHasM(eMyGeomType) && !wkbHasM(eGeomInType))
                    {
                        poGeometry->setMeasured(TRUE);
                    }
                    else if (!wkbHasM(eMyGeomType) && wkbHasM(eGeomInType))
                    {
                        poGeometry->setMeasured(FALSE);
                    }
                }
            }

            poFeature->SetGeometryDirectly(poGeometry);
        }
        else if (psShape != nullptr)
        {
            SHPDestroyObject(psShape);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Fetch feature attributes to OGRFeature fields.                  */
    /* -------------------------------------------------------------------- */

    for (int iField = 0; hDBF != nullptr && iField < poDefn->GetFieldCount();
         iField++)
    {
        const OGRFieldDefn *const poFieldDefn = poDefn->GetFieldDefn(iField);
        if (poFieldDefn->IsIgnored())
            continue;

        switch (poFieldDefn->GetType())
        {
            case OFTString:
            {
                const char *const pszFieldVal =
                    DBFReadStringAttribute(hDBF, iShape, iField);
                if (pszFieldVal != nullptr && pszFieldVal[0] != '\0')
                {
                    if (pszSHPEncoding[0] != '\0')
                    {
                        char *const pszUTF8Field = CPLRecode(
                            pszFieldVal, pszSHPEncoding, CPL_ENC_UTF8);
                        poFeature->SetField(iField, pszUTF8Field);
                        CPLFree(pszUTF8Field);
                    }
                    else
                        poFeature->SetField(iField, pszFieldVal);
                }
                else
                {
                    poFeature->SetFieldNull(iField);
                }
                break;
            }
            case OFTInteger:
            case OFTInteger64:
            case OFTReal:
            {
                if (DBFIsAttributeNULL(hDBF, iShape, iField))
                {
                    poFeature->SetFieldNull(iField);
                }
                else
                {
                    if (poFieldDefn->GetSubType() == OFSTBoolean)
                    {
                        const char *pszVal =
                            DBFReadLogicalAttribute(hDBF, iShape, iField);
                        poFeature->SetField(
                            iField, pszVal[0] == 'T' || pszVal[0] == 't' ||
                                            pszVal[0] == 'Y' || pszVal[0] == 'y'
                                        ? 1
                                        : 0);
                    }
                    else
                    {
                        const char *pszVal =
                            DBFReadStringAttribute(hDBF, iShape, iField);
                        poFeature->SetField(iField, pszVal);
                    }
                }
                break;
            }
            case OFTDate:
            {
                if (DBFIsAttributeNULL(hDBF, iShape, iField))
                {
                    poFeature->SetFieldNull(iField);
                    continue;
                }

                const char *const pszDateValue =
                    DBFReadStringAttribute(hDBF, iShape, iField);

                OGRField sFld;
                memset(&sFld, 0, sizeof(sFld));

                if (strlen(pszDateValue) >= 10 && pszDateValue[2] == '/' &&
                    pszDateValue[5] == '/')
                {
                    sFld.Date.Month =
                        static_cast<GByte>(atoi(pszDateValue + 0));
                    sFld.Date.Day = static_cast<GByte>(atoi(pszDateValue + 3));
                    sFld.Date.Year =
                        static_cast<GInt16>(atoi(pszDateValue + 6));
                }
                else
                {
                    const int nFullDate = atoi(pszDateValue);
                    sFld.Date.Year = static_cast<GInt16>(nFullDate / 10000);
                    sFld.Date.Month =
                        static_cast<GByte>((nFullDate / 100) % 100);
                    sFld.Date.Day = static_cast<GByte>(nFullDate % 100);
                }

                poFeature->SetField(iField, &sFld);
            }
            break;

            default:
                CPLAssert(false);
        }
    }

    if (poFeature != nullptr)
        poFeature->SetFID(iShape);

    return poFeature;
}

/************************************************************************/
/*                             GrowField()                              */
/************************************************************************/

static OGRErr GrowField(DBFHandle hDBF, int iField, OGRFieldDefn *poFieldDefn,
                        int nNewSize)
{
    char szFieldName[20] = {};
    int nOriWidth = 0;
    int nPrecision = 0;
    DBFGetFieldInfo(hDBF, iField, szFieldName, &nOriWidth, &nPrecision);

    CPLDebug("SHAPE", "Extending field %d (%s) from %d to %d characters",
             iField, poFieldDefn->GetNameRef(), nOriWidth, nNewSize);

    const char chNativeType = DBFGetNativeFieldType(hDBF, iField);
    if (!DBFAlterFieldDefn(hDBF, iField, szFieldName, chNativeType, nNewSize,
                           nPrecision))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Extending field %d (%s) from %d to %d characters failed",
                 iField, poFieldDefn->GetNameRef(), nOriWidth, nNewSize);
        return OGRERR_FAILURE;
    }

    auto oTemporaryUnsealer(poFieldDefn->GetTemporaryUnsealer());
    poFieldDefn->SetWidth(nNewSize);
    return OGRERR_NONE;
}

/************************************************************************/
/*                         SHPWriteOGRFeature()                         */
/*                                                                      */
/*      Write to an existing feature in a shapefile, or create a new    */
/*      feature.                                                        */
/************************************************************************/

OGRErr SHPWriteOGRFeature(SHPHandle hSHP, DBFHandle hDBF,
                          OGRFeatureDefn *poDefn, OGRFeature *poFeature,
                          const char *pszSHPEncoding,
                          bool *pbTruncationWarningEmitted, bool bRewind)

{
    /* -------------------------------------------------------------------- */
    /*      Write the geometry.                                             */
    /* -------------------------------------------------------------------- */
    if (hSHP != nullptr)
    {
        const OGRErr eErr = SHPWriteOGRObject(
            hSHP, static_cast<int>(poFeature->GetFID()),
            poFeature->GetGeometryRef(), bRewind, poDefn->GetGeomType());
        if (eErr != OGRERR_NONE)
            return eErr;
    }

    /* -------------------------------------------------------------------- */
    /*      If there is no DBF, the job is done now.                        */
    /* -------------------------------------------------------------------- */
    if (hDBF == nullptr)
    {
        /* --------------------------------------------------------------------
         */
        /*      If this is a new feature, establish its feature id. */
        /* --------------------------------------------------------------------
         */
        if (hSHP != nullptr && poFeature->GetFID() == OGRNullFID)
            poFeature->SetFID(hSHP->nRecords - 1);

        return OGRERR_NONE;
    }

    /* -------------------------------------------------------------------- */
    /*      If this is a new feature, establish its feature id.             */
    /* -------------------------------------------------------------------- */
    if (poFeature->GetFID() == OGRNullFID)
        poFeature->SetFID(DBFGetRecordCount(hDBF));

    /* -------------------------------------------------------------------- */
    /*      If this is the first feature to be written, verify that we      */
    /*      have at least one attribute in the DBF file.  If not, create    */
    /*      a dummy FID attribute to satisfy the requirement that there     */
    /*      be at least one attribute.                                      */
    /* -------------------------------------------------------------------- */
    if (DBFGetRecordCount(hDBF) == 0 && DBFGetFieldCount(hDBF) == 0)
    {
        CPLDebug(
            "OGR",
            "Created dummy FID field for shapefile since schema is empty.");
        DBFAddField(hDBF, "FID", FTInteger, 11, 0);
    }

    /* -------------------------------------------------------------------- */
    /*      Write out dummy field value if it exists.                       */
    /* -------------------------------------------------------------------- */
    if (poDefn->GetFieldCount() == 0)
    {
        if (DBFGetFieldCount(hDBF) == 1)
        {
            DBFWriteIntegerAttribute(hDBF,
                                     static_cast<int>(poFeature->GetFID()), 0,
                                     static_cast<int>(poFeature->GetFID()));
        }
        else if (DBFGetFieldCount(hDBF) == 0)
        {
            // Far from being nominal... Could happen if deleting all fields
            // of a DBF with rows
            DBFWriteAttributeDirectly(
                hDBF, static_cast<int>(poFeature->GetFID()), -1, nullptr);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Write all the fields.                                           */
    /* -------------------------------------------------------------------- */
    for (int iField = 0; iField < poDefn->GetFieldCount(); iField++)
    {
        if (!poFeature->IsFieldSetAndNotNull(iField))
        {
            DBFWriteNULLAttribute(hDBF, static_cast<int>(poFeature->GetFID()),
                                  iField);
            continue;
        }

        OGRFieldDefn *const poFieldDefn = poDefn->GetFieldDefn(iField);

        switch (poFieldDefn->GetType())
        {
            case OFTString:
            {
                const char *pszStr = poFeature->GetFieldAsString(iField);
                char *pszEncoded = nullptr;
                if (pszSHPEncoding[0] != '\0')
                {
                    pszEncoded =
                        CPLRecode(pszStr, CPL_ENC_UTF8, pszSHPEncoding);
                    pszStr = pszEncoded;
                }

                int nStrLen = static_cast<int>(strlen(pszStr));
                if (nStrLen > OGR_DBF_MAX_FIELD_WIDTH)
                {
                    if (!(*pbTruncationWarningEmitted))
                    {
                        *pbTruncationWarningEmitted = true;
                        CPLError(
                            CE_Warning, CPLE_AppDefined,
                            "Value '%s' of field %s has been truncated to %d "
                            "characters.  This warning will not be emitted any "
                            "more for that layer.",
                            poFeature->GetFieldAsString(iField),
                            poFieldDefn->GetNameRef(), OGR_DBF_MAX_FIELD_WIDTH);
                    }

                    nStrLen = OGR_DBF_MAX_FIELD_WIDTH;

                    if (pszEncoded != nullptr &&  // For Coverity.
                        EQUAL(pszSHPEncoding, CPL_ENC_UTF8))
                    {
                        // Truncate string by making sure we don't cut in the
                        // middle of a UTF-8 multibyte character
                        // Continuation bytes of such characters are of the form
                        // 10xxxxxx (0x80), whereas single-byte are 0xxxxxxx
                        // and the start of a multi-byte is 11xxxxxx
                        const char *p = pszStr + nStrLen;
                        while (nStrLen > 0)
                        {
                            if ((*p & 0xc0) != 0x80)
                            {
                                break;
                            }

                            nStrLen--;
                            p--;
                        }

                        pszEncoded[nStrLen] = 0;
                    }
                }

                if (nStrLen > poFieldDefn->GetWidth())
                {
                    if (GrowField(hDBF, iField, poFieldDefn, nStrLen) !=
                        OGRERR_NONE)
                    {
                        CPLFree(pszEncoded);
                        return OGRERR_FAILURE;
                    }
                }

                DBFWriteStringAttribute(hDBF,
                                        static_cast<int>(poFeature->GetFID()),
                                        iField, pszStr);

                CPLFree(pszEncoded);
                break;
            }
            case OFTInteger:
            case OFTInteger64:
            {
                if (poFieldDefn->GetSubType() == OFSTBoolean)
                {
                    DBFWriteAttributeDirectly(
                        hDBF, static_cast<int>(poFeature->GetFID()), iField,
                        poFeature->GetFieldAsInteger(iField) ? "T" : "F");
                }
                else
                {
                    char szValue[32] = {};
                    const int nFieldWidth = poFieldDefn->GetWidth();
                    snprintf(szValue, sizeof(szValue),
                             "%*" CPL_FRMT_GB_WITHOUT_PREFIX "d",
                             std::min(nFieldWidth,
                                      static_cast<int>(sizeof(szValue)) - 1),
                             poFeature->GetFieldAsInteger64(iField));

                    const int nStrLen = static_cast<int>(strlen(szValue));
                    if (nStrLen > nFieldWidth)
                    {
                        if (GrowField(hDBF, iField, poFieldDefn, nStrLen) !=
                            OGRERR_NONE)
                        {
                            return OGRERR_FAILURE;
                        }
                    }

                    DBFWriteAttributeDirectly(
                        hDBF, static_cast<int>(poFeature->GetFID()), iField,
                        szValue);
                }

                break;
            }

            case OFTReal:
            {
                const double dfVal = poFeature->GetFieldAsDouble(iField);
                // IEEE754 doubles can store exact values of all integers
                // below 2^53.
                if (poFieldDefn->GetPrecision() == 0 &&
                    fabs(dfVal) > (static_cast<GIntBig>(1) << 53))
                {
                    static int nCounter = 0;
                    if (nCounter <= 10)
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Value %.17g of field %s with 0 decimal of "
                                 "feature " CPL_FRMT_GIB
                                 " is bigger than 2^53. "
                                 "Precision loss likely occurred or going to "
                                 "happen.%s",
                                 dfVal, poFieldDefn->GetNameRef(),
                                 poFeature->GetFID(),
                                 (nCounter == 10) ? " This warning will not be "
                                                    "emitted anymore."
                                                  : "");
                        nCounter++;
                    }
                }
                int ret = DBFWriteDoubleAttribute(
                    hDBF, static_cast<int>(poFeature->GetFID()), iField, dfVal);
                if (!ret)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Value %.17g of field %s of feature " CPL_FRMT_GIB
                             " not "
                             "successfully written. Possibly due to too larger "
                             "number "
                             "with respect to field width",
                             dfVal, poFieldDefn->GetNameRef(),
                             poFeature->GetFID());
                }
                break;
            }
            case OFTDate:
            {
                const OGRField *const psField =
                    poFeature->GetRawFieldRef(iField);

                if (psField->Date.Year < 0 || psField->Date.Year > 9999)
                {
                    CPLError(
                        CE_Warning, CPLE_NotSupported,
                        "Year < 0 or > 9999 is not a valid date for shapefile");
                }
                else if (psField->Date.Year == 0 && psField->Date.Month == 0 &&
                         psField->Date.Day == 0)
                {
                    DBFWriteNULLAttribute(
                        hDBF, static_cast<int>(poFeature->GetFID()), iField);
                }
                else
                {
                    DBFWriteIntegerAttribute(
                        hDBF, static_cast<int>(poFeature->GetFID()), iField,
                        psField->Date.Year * 10000 + psField->Date.Month * 100 +
                            psField->Date.Day);
                }
            }
            break;

            default:
            {
                // Ignore fields of other types.
                break;
            }
        }
    }

    return OGRERR_NONE;
}
