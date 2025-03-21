#!/usr/bin/env pytest
# -*- coding: utf-8 -*-
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test OGR S-57 driver functionality.
# Author:   Frank Warmerdam <warmerdam@pobox.com>
#
###############################################################################
# Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2011-2013, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import os
import shutil

import gdaltest
import ogrtest
import pytest

from osgeo import gdal, ogr

pytestmark = pytest.mark.require_driver("S57")

###############################################################################
@pytest.fixture(autouse=True, scope="module")
def module_disable_exceptions():
    with gdaltest.disable_exceptions():
        yield


###############################################################################
# Verify we can open the test file.


def test_ogr_s57_check_layers(fname="data/s57/1B5X02NE.000"):

    s57_ds = ogr.Open(fname)
    assert s57_ds is not None, "failed to open test file."

    ###############################################################################
    # Verify we have the set of expected layers and that some rough information
    # matches our expectations.

    layer_list = [
        ("DSID", ogr.wkbNone, 1),
        ("COALNE", ogr.wkbUnknown, 1),
        ("DEPARE", ogr.wkbUnknown, 4),
        ("DEPCNT", ogr.wkbUnknown, 4),
        ("LNDARE", ogr.wkbUnknown, 1),
        ("LNDELV", ogr.wkbUnknown, 2),
        ("SBDARE", ogr.wkbUnknown, 2),
        ("SLCONS", ogr.wkbUnknown, 1),
        ("SLOTOP", ogr.wkbUnknown, 1),
        ("SOUNDG", ogr.wkbMultiPoint25D, 2),
        ("M_COVR", ogr.wkbPolygon, 1),
        ("M_NSYS", ogr.wkbPolygon, 1),
        ("M_QUAL", ogr.wkbPolygon, 1),
    ]

    assert s57_ds.GetLayerCount() == len(
        layer_list
    ), "Did not get expected number of layers, likely cannot find support files."

    for i, lyr_info in enumerate(layer_list):
        lyr = s57_ds.GetLayer(i)

        assert (
            lyr.GetName() == lyr_info[0]
        ), "Expected layer %d to be %s but it was %s." % (
            i + 1,
            lyr_info[0],
            lyr.GetName(),
        )

        count = lyr.GetFeatureCount(force=1)
        assert count == lyr_info[2], "Expected %d features in layer %s, but got %d." % (
            lyr_info[2],
            lyr_info[0],
            count,
        )

        assert (
            lyr.GetLayerDefn().GetGeomType() == lyr_info[1]
        ), "Expected %d layer type in layer %s, but got %d." % (
            lyr_info[1],
            lyr_info[0],
            lyr.GetLayerDefn().GetGeomType(),
        )


###############################################################################
# Check the COALNE feature.


def test_ogr_s57_COALNE(fname="data/s57/1B5X02NE.000"):

    s57_ds = ogr.Open(fname)

    feat = s57_ds.GetLayerByName("COALNE").GetNextFeature()

    assert feat is not None, "Did not get expected COALNE feature at all."

    assert (
        feat.GetField("RCID") == 1
        and feat.GetField("LNAM") == "FFFF7F4F0FB002D3"
        and feat.GetField("OBJL") == 30
        and feat.GetField("AGEN") == 65535
    ), "COALNE: did not get expected attributes"

    wkt = "LINESTRING (60.97683400 -32.49442600,60.97718200 -32.49453800,60.97742400 -32.49477400,60.97774800 -32.49504000,60.97791600 -32.49547200,60.97793000 -32.49581800,60.97794400 -32.49617800,60.97804400 -32.49647600,60.97800200 -32.49703800,60.97800200 -32.49726600,60.97805800 -32.49749400,60.97812800 -32.49773200,60.97827000 -32.49794800,60.97910200 -32.49848600,60.97942600 -32.49866600)"

    ogrtest.check_feature_geometry(feat, wkt)


###############################################################################
# Check the M_QUAL feature.


def test_ogr_s57_M_QUAL(fname="data/s57/1B5X02NE.000"):

    s57_ds = ogr.Open(fname)

    feat = s57_ds.GetLayerByName("M_QUAL").GetNextFeature()

    assert feat is not None, "Did not get expected M_QUAL feature at all."

    assert (
        feat.GetField("RCID") == 15
        and feat.GetField("OBJL") == 308
        and feat.GetField("AGEN") == 65535
    ), "M_QUAL: did not get expected attributes"

    wkt = "POLYGON ((60.97683400 -32.49534000,60.97683400 -32.49762000,60.97683400 -32.49866600,60.97869000 -32.49866600,60.97942600 -32.49866600,60.98215200 -32.49866600,60.98316600 -32.49866600,60.98316600 -32.49755800,60.98316600 -32.49477000,60.98316600 -32.49350000,60.98146800 -32.49350000,60.98029800 -32.49350000,60.97947400 -32.49350000,60.97901600 -32.49350000,60.97683400 -32.49350000,60.97683400 -32.49442600,60.97683400 -32.49469800,60.97683400 -32.49534000))"

    ogrtest.check_feature_geometry(feat, wkt)


###############################################################################
# Check the SOUNDG feature.


def test_ogr_s57_SOUNDG(fname="data/s57/1B5X02NE.000"):

    s57_ds = ogr.Open(fname)

    feat = s57_ds.GetLayerByName("SOUNDG").GetNextFeature()

    assert feat is not None, "Did not get expected SOUNDG feature at all."

    assert (
        feat.GetField("RCID") == 20
        and feat.GetField("OBJL") == 129
        and feat.GetField("AGEN") == 65535
    ), "SOUNDG: did not get expected attributes"

    assert feat.GetField("QUASOU") == ["1"]

    wkt = "MULTIPOINT (60.98164400 -32.49449000 3.400,60.98134400 -32.49642400 1.400,60.97814200 -32.49487400 -3.200,60.98071200 -32.49519600 1.200)"

    ogrtest.check_feature_geometry(feat, wkt)


###############################################################################
# Test reading features from dataset with some double byte attributes. (#1526)


def test_ogr_s57_double_byte_attrs():

    ds = ogr.Open("data/s57/bug1526.000")

    feat = ds.GetLayerByName("FOGSIG").GetNextFeature()

    assert feat is not None, "Did not get expected FOGSIG feature at all."

    assert (
        feat.GetField("INFORM")
        == "During South winds nautophone is not always heard in S direction from lighthouse"
        and len(feat.GetField("NINFOM")) >= 1
    ), "FOGSIG: did not get expected attributes"


###############################################################################
# Test handling of a dataset with a multilinestring feature (#2147).


def test_ogr_s57_multilinestring():

    ds = ogr.Open("data/s57/bug2147_3R7D0889.000")

    feat = ds.GetLayerByName("ROADWY").GetNextFeature()

    assert feat is not None, "Did not get expected feature at all."

    exp_wkt = "MULTILINESTRING ((22.5659615 44.5541942,22.5652045 44.5531651,22.5654315 44.5517774,22.5663008 44.5510096,22.5656187 44.5500822,22.5654462 44.5495941,22.5637522 44.5486793,22.563408 44.5477286,22.5654087 44.5471198,22.5670327 44.5463937,22.5667729 44.5456512,22.5657613 44.544027,22.5636273 44.5411638,22.5623421 44.5400398,22.559403 44.5367489,22.5579112 44.534544,22.5566466 44.5309514,22.5563888 44.5295231,22.5549946 44.5285915,22.5541939 44.5259331,22.5526434 44.5237888),(22.5656187 44.5500822,22.5670219 44.5493519,22.5684077 44.5491452),(22.5350702 44.4918838,22.5329111 44.4935825,22.5318719 44.4964337,22.5249608 44.5027089,22.5254709 44.5031914,22.5295138 44.5052214,22.5331359 44.5077711,22.5362468 44.5092751,22.5408091 44.5115306,22.5441312 44.5127374,22.5461053 44.5132675,22.5465694 44.5149956),(22.5094658 44.4989464,22.5105135 44.4992481,22.5158217 44.4994216,22.5206067 44.4998907,22.523096 44.5009452,22.5249608 44.5027089),(22.5762962 44.4645734,22.5767653 44.4773213,22.5769802 44.4796618,22.5775485 44.4815858,22.5762434 44.4842544,22.5765836 44.4855091,22.5775087 44.4865991,22.5769145 44.4879336,22.5708196 44.4910838,22.5694028 44.4930833,22.5692354 44.4958977),(22.5763768 44.5029527,22.5799605 44.501315,22.5831172 44.5007428,22.584524 44.4999964,22.5848604 44.4999039),(22.5731362 44.5129105,22.5801378 44.5261859,22.5825748 44.5301187),(22.5093748 44.5311182,22.5107969 44.5285258,22.5108905 44.5267978,22.5076679 44.5223309))"

    ogrtest.check_feature_geometry(feat, exp_wkt)


###############################################################################
# Run test_ogrsf


def test_ogr_s57_test_ogrsf():

    import test_cli_utilities

    if test_cli_utilities.get_test_ogrsf_path() is None:
        pytest.skip()

    ret = gdaltest.runexternal(
        test_cli_utilities.get_test_ogrsf_path() + " -ro data/s57/1B5X02NE.000"
    )

    assert ret.find("INFO") != -1 and ret.find("ERROR") == -1


###############################################################################
# Test S57 to S57 conversion


def test_ogr_s57_write_1(tmp_path):

    dst_filename = tmp_path / "ogr_s57_9.000"

    with gdal.config_option(
        "OGR_S57_OPTIONS", "RETURN_PRIMITIVES=ON,RETURN_LINKAGES=ON,LNAM_REFS=ON"
    ):
        ds = ogr.GetDriverByName("S57").CreateDataSource(dst_filename)
        src_ds = ogr.Open("data/s57/1B5X02NE.000")

    for src_lyr in src_ds:
        if src_lyr.GetName() == "DSID":
            continue
        lyr = ds.GetLayerByName(src_lyr.GetName())
        for src_feat in src_lyr:
            feat = ogr.Feature(lyr.GetLayerDefn())
            feat.SetFrom(src_feat)
            lyr.CreateFeature(feat)
    src_ds = None
    ds = None

    test_ogr_s57_check_layers(dst_filename)
    test_ogr_s57_COALNE(dst_filename)
    test_ogr_s57_M_QUAL(dst_filename)
    test_ogr_s57_SOUNDG(dst_filename)


def test_ogr_s57_write_2(tmp_path):

    dst_filename = tmp_path / "ogr_s57_9.000"

    with gdal.config_option(
        "OGR_S57_OPTIONS", "RETURN_PRIMITIVES=ON,RETURN_LINKAGES=ON,LNAM_REFS=ON"
    ):
        gdal.VectorTranslate(
            dst_filename,
            "data/s57/1B5X02NE.000",
            options="-f S57 IsolatedNode ConnectedNode Edge Face M_QUAL SOUNDG",
        )

    ds = gdal.OpenEx(dst_filename, open_options=["RETURN_PRIMITIVES=ON"])

    assert ds is not None

    assert ds.GetLayerByName("IsolatedNode") is not None

    test_ogr_s57_M_QUAL(dst_filename)
    test_ogr_s57_SOUNDG(dst_filename)


###############################################################################
# Test opening a fake very small S57 file


def test_ogr_s57_10():

    ds = ogr.Open("data/s57/fake_s57.000")
    lyr = ds.GetLayer(0)
    f = lyr.GetNextFeature()
    assert f["DSID_EXPP"] == 2


###############################################################################
# Test opening a fake very small S57 file with ISO8211 record with zero length,
# using variant (C.1.5.1) logic.


def test_ogr_s57_11():

    ds = ogr.Open("data/s57/fake_s57_variant_C151.000")
    lyr = ds.GetLayer(0)
    f = lyr.GetNextFeature()
    assert f["DSID_EXPP"] == 2


###############################################################################
# Test decoding of Dutch inland ENCs (#3881).


def test_ogr_s57_online_1():

    gdaltest.download_or_skip("ftp://sdg.ivs90.nl/ENC/1R5MK050.000", "1R5MK050.000")

    ds = ogr.Open("tmp/cache/1R5MK050.000")
    assert ds is not None

    lyr = ds.GetLayerByName("BUISGL")
    feat = lyr.GetNextFeature()

    assert feat is not None, "Did not get expected feature at all."

    exp_wkt = "POLYGON ((5.6666667 53.0279027,5.6666667 53.0281667,5.6667012 53.0281685,5.666673 53.0282377,5.666788 53.0282616,5.6669018 53.0281507,5.6668145 53.0281138,5.6668121 53.0280649,5.6666686 53.0280248,5.6666713 53.0279647,5.6667572 53.0279713,5.6667568 53.0279089,5.6666667 53.0279027))"

    ogrtest.check_feature_geometry(feat, exp_wkt)

    feat = None

    ds = None


###############################################################################
# Test with ENC 3.0 TDS - tile without updates.


def test_ogr_s57_online_2(tmp_path):

    gdaltest.download_or_skip(
        "http://download.osgeo.org/gdal/data/s57/enctds/GB5X01SW.000", "GB5X01SW.000"
    )

    gdaltest.clean_tmp()
    shutil.copy("tmp/cache/GB5X01SW.000", tmp_path)
    ds = ogr.Open(tmp_path / "GB5X01SW.000")
    assert ds is not None

    lyr = ds.GetLayerByName("LIGHTS")
    feat = lyr.GetFeature(542)

    assert feat is not None, "Did not get expected feature at all."

    assert feat.rver == 1, "Did not get expected RVER value (%d)." % feat.rver

    lyr = ds.GetLayerByName("BOYCAR")
    feat = lyr.GetFeature(975)
    assert feat is None, "unexpected got feature id 975 before update!"

    feat = None

    ds = None


###############################################################################
# Test with ENC 3.0 TDS - tile with updates.


# This test appears to have had a typo since it was added in bf50149
# In that commit, the file GB5X01SW.001 is added, but GB5X01SW.000 leftover
# from the previous tests is opened.
@pytest.mark.xfail()
def test_ogr_s57_online_3(tmp_path):

    gdaltest.download_or_skip(
        "http://download.osgeo.org/gdal/data/s57/enctds/GB5X01SW.001", "GB5X01SW.001"
    )

    shutil.copy("tmp/cache/GB5X01SW.001", tmp_path)
    ds = ogr.Open(tmp_path / "GB5X01SW.001")
    assert ds is not None

    lyr = ds.GetLayerByName("LIGHTS")
    feat = lyr.GetFeature(542)

    assert feat is not None, "Did not get expected feature at all."

    assert feat.rver == 2, "Did not get expected RVER value (%d)." % feat.rver

    lyr = ds.GetLayerByName("BOYCAR")
    feat = lyr.GetFeature(975)
    assert feat is not None, "unexpected did not get feature id 975 " "after update!"

    feat = None

    ds = None

    gdaltest.clean_tmp()


###############################################################################
# Test ENC LL2 (#5048)


def test_ogr_s57_online_4():

    gdaltest.download_or_skip(
        "http://www1.kaiho.mlit.go.jp/KOKAI/ENC/images/sample/sample.zip", "sample.zip"
    )

    try:
        os.stat("tmp/cache/ENC_ROOT/JP34NC94.000")
    except OSError:
        try:
            gdaltest.unzip("tmp/cache", "tmp/cache/sample.zip")
            try:
                os.stat("tmp/cache/ENC_ROOT/JP34NC94.000")
            except OSError:
                pytest.skip()
        except OSError:
            pytest.skip()

    with gdal.config_option(
        "OGR_S57_OPTIONS", "RETURN_PRIMITIVES=ON,RETURN_LINKAGES=ON,LNAM_REFS=ON"
    ):
        ds = ogr.Open("tmp/cache/ENC_ROOT/JP34NC94.000")
    lyr = ds.GetLayerByName("LNDMRK")
    for feat in lyr:
        feat.NOBJNM


###############################################################################
# Test updates of DSID (#2498)


def test_ogr_s57_update_dsid():

    ds = ogr.Open("data/s57/fake_s57_update_dsid.000")
    lyr = ds.GetLayerByName("DSID")
    f = lyr.GetNextFeature()
    assert f["DSID_EDTN"] == "0"
    assert f["DSID_UPDN"] == "1"
    assert f["DSID_UADT"] == "20190211"
    assert f["DSID_ISDT"] == "20190212"


###############################################################################
# Test fix for https://github.com/OSGeo/gdal/issues/5461#issuecomment-1075393495


def test_ogr_s57_more_than_255_updates_to_feature():

    gdaltest.download_or_skip(
        "https://www.charts.noaa.gov/ENCs/US5ME51M.zip", "US5ME51M.zip"
    )

    try:
        os.stat("tmp/cache/US5ME51M")
    except OSError:
        try:
            gdaltest.unzip("tmp/cache/US5ME51M", "tmp/cache/US5ME51M.zip")
            try:
                os.stat("tmp/cache/US5ME51M")
            except OSError:
                pytest.skip()
        except Exception:
            pytest.skip()

    gdal.ErrorReset()
    ds = ogr.Open("tmp/cache/US5ME51M/ENC_ROOT/US5ME51M/US5ME51M.000")
    assert ds is not None
    assert gdal.GetLastErrorMsg() == ""
