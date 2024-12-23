#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test ILWIS format driver.
# Author:   Frank Warmerdam <warmerdam@pobox.com>
#
###############################################################################
# Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
#
# SPDX-License-Identifier: MIT
###############################################################################


import gdaltest

###############################################################################
# Perform simple read test.


def test_ilwis_1():

    tst = gdaltest.GDALTest("ilwis", "ilwis/LanduseSmall.mpr", 1, 2351)

    srs = """PROJCS["UTM",
    GEOGCS["PSAD56",
        DATUM["Provisional_South_American_Datum_1956",
            SPHEROID["International 1924",6378388,297,
                AUTHORITY["EPSG","7022"]],
            AUTHORITY["EPSG","6248"]],
        PRIMEM["Greenwich",0,
            AUTHORITY["EPSG","8901"]],
        UNIT["degree",0.0174532925199433,
            AUTHORITY["EPSG","9122"]],
        AUTHORITY["EPSG","4248"]],
    PROJECTION["Transverse_Mercator"],
    PARAMETER["latitude_of_origin",0],
    PARAMETER["central_meridian",-69],
    PARAMETER["scale_factor",0.9996],
    PARAMETER["false_easting",500000],
    PARAMETER["false_northing",10000000],
    UNIT["Meter",1],
    AXIS["Easting",EAST],
    AXIS["Northing",NORTH]]"""

    gt = (795480, 20, 0, 8090520, 0, -20)

    tst.testOpen(check_gt=gt, check_prj=srs)


###############################################################################
# copy byte data and verify.


def test_ilwis_2():

    tst = gdaltest.GDALTest("ilwis", "byte.tif", 1, 4672)

    tst.testCreateCopy(check_srs=1, check_gt=1, new_filename="tmp/byte.mpr")


###############################################################################
# copy floating point data and use Create interface.


def test_ilwis_3():

    tst = gdaltest.GDALTest("ilwis", "hfa/float.img", 1, 23529)

    tst.testCreate(new_filename="tmp/float.mpr", out_bands=1)


###############################################################################
# Try multi band dataset.


def test_ilwis_4():

    tst = gdaltest.GDALTest("ilwis", "rgbsmall.tif", 2, 21053)

    tst.testCreate(new_filename="tmp/rgb.mpl", check_minmax=0, out_bands=3)


###############################################################################
# Test vsi in-memory support.


def test_ilwis_5():

    tst = gdaltest.GDALTest("ilwis", "byte.tif", 1, 4672)

    tst.testCreateCopy(
        check_srs=1, check_gt=1, vsimem=1, new_filename="/vsimem/ilwis/byte.mpr"
    )


###############################################################################
# Cleanup.
#
# Currently the ILWIS driver does not keep track of the files that are
# part of the dataset properly, so we can't automatically clean them up
# properly. So we do the brute force approach...


def test_ilwis_cleanup():
    gdaltest.clean_tmp()
