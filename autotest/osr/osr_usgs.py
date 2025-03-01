#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test some USGS specific translation issues.
# Author:   Andrey Kiselev, dron@remotesensing.org
#
###############################################################################
# Copyright (c) 2004, Andrey Kiselev <dron@remotesensing.org>
#
# SPDX-License-Identifier: MIT
###############################################################################


import pytest

from osgeo import gdal, osr

###############################################################################
# Test the osr.SpatialReference.ImportFromUSGS() function.
#


def test_osr_usgs_1():

    srs = osr.SpatialReference()
    srs.ImportFromUSGS(
        8,
        0,
        (
            0.0,
            0.0,
            gdal.DecToPackedDMS(47.0),
            gdal.DecToPackedDMS(62.0),
            gdal.DecToPackedDMS(45.0),
            gdal.DecToPackedDMS(54.5),
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
        ),
        15,
    )

    assert (
        srs.GetProjParm(osr.SRS_PP_STANDARD_PARALLEL_1)
        == pytest.approx(47.0, abs=0.0000005)
        and srs.GetProjParm(osr.SRS_PP_STANDARD_PARALLEL_2)
        == pytest.approx(62.0, abs=0.0000005)
        and srs.GetProjParm(osr.SRS_PP_LATITUDE_OF_CENTER)
        == pytest.approx(54.5, abs=0.0000005)
        and srs.GetProjParm(osr.SRS_PP_LONGITUDE_OF_CENTER)
        == pytest.approx(45.0, abs=0.0000005)
        and srs.GetProjParm(osr.SRS_PP_FALSE_EASTING)
        == pytest.approx(0.0, abs=0.0000005)
        and srs.GetProjParm(osr.SRS_PP_FALSE_NORTHING)
        == pytest.approx(0.0, abs=0.0000005)
    ), "Can not import Equidistant Conic projection."


###############################################################################
# Test the osr.SpatialReference.ExportToUSGS() function.
#


def test_osr_usgs_2():

    srs = osr.SpatialReference()
    srs.ImportFromWkt(
        """PROJCS["unnamed",GEOGCS["NAD27",\
    DATUM["North_American_Datum_1927",\
    SPHEROID["Clarke 1866",6378206.4,294.9786982139006,\
    AUTHORITY["EPSG","7008"]],AUTHORITY["EPSG","6267"]],\
    PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],\
    AUTHORITY["EPSG","4267"]],PROJECTION["Lambert_Conformal_Conic_2SP"],\
    PARAMETER["standard_parallel_1",33.90363402777778],\
    PARAMETER["standard_parallel_2",33.62529002777778],\
    PARAMETER["latitude_of_origin",33.76446202777777],\
    PARAMETER["central_meridian",-117.4745428888889],\
    PARAMETER["false_easting",0],PARAMETER["false_northing",0],\
    UNIT["metre",1,AUTHORITY["EPSG","9001"]]]"""
    )

    (proj_code, _, params, datum_code) = srs.ExportToUSGS()

    assert (
        proj_code == 4
        and datum_code == 0
        and gdal.PackedDMSToDec(params[2]) == pytest.approx(33.90363403, abs=0.0000005)
        and gdal.PackedDMSToDec(params[3]) == pytest.approx(33.62529003, abs=0.0000005)
        and gdal.PackedDMSToDec(params[4]) == pytest.approx(-117.4745429, abs=0.0000005)
        and gdal.PackedDMSToDec(params[5]) == pytest.approx(33.76446203, abs=0.0000005)
    ), "Can not import Lambert Conformal Conic projection."


###############################################################################
# Test the osr.SpatialReference.ImportFromUSGS() function with WGS 84
#


def test_osr_usgs_wgs84():

    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    (proj_code, zone, params, datum_code) = srs.ExportToUSGS()

    srs2 = osr.SpatialReference()
    srs2.ImportFromUSGS(proj_code, zone, params, datum_code)

    assert srs2.IsSame(srs)
    assert srs2.GetAuthorityCode(None) == "32631"
