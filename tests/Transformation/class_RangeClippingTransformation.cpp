/*
    (C) Copyright 2016 CEA LIST. All Rights Reserved.
    Contributor(s): Olivier BICHLER (olivier.bichler@cea.fr)

    This software is governed by the CeCILL-C license under French law and
    abiding by the rules of distribution of free software.  You can  use,
    modify and/ or redistribute the software under the terms of the CeCILL-C
    license as circulated by CEA, CNRS and INRIA at the following URL
    "http://www.cecill.info".

    As a counterpart to the access to the source code and  rights to copy,
    modify and redistribute granted by the license, users are provided only
    with a limited warranty  and the software's author,  the holder of the
    economic rights,  and the successive licensors  have only  limited
    liability.

    The fact that you are presently reading this means that you have had
    knowledge of the CeCILL-C license and that you accept its terms.
*/

#include "Transformation/RangeClippingTransformation.hpp"
#include "utils/UnitTest.hpp"
#include "utils/Utils.hpp"

using namespace N2D2;

TEST_DATASET(RangeClippingTransformation,
             apply,
             (bool color, double rangeMin, double rangeMax),
             std::make_tuple(true, 0.0, 0.0),
             std::make_tuple(true, 128.0, 0.0),
             std::make_tuple(true, 0.0, 128.0),
             std::make_tuple(true, 100.0, 200.0),
             std::make_tuple(false, 0.0, 0.0),
             std::make_tuple(false, 128.0, 0.0),
             std::make_tuple(false, 0.0, 128.0),
             std::make_tuple(false, 100.0, 200.0))
{
    RangeClippingTransformation trans;
    trans.setParameter("RangeMin", rangeMin);
    trans.setParameter("RangeMax", rangeMax);

    cv::Mat img
        = cv::imread("tests_data/Lenna.png",
#if CV_MAJOR_VERSION >= 3
                     (color) ? cv::IMREAD_COLOR : cv::IMREAD_GRAYSCALE);
#else
                     (color) ? CV_LOAD_IMAGE_COLOR : CV_LOAD_IMAGE_GRAYSCALE);
#endif

    if (!img.data)
        throw std::runtime_error(
            "Could not open or find image: tests_data/Lenna.png");

    trans.apply(img);

    std::ostringstream fileName;
    fileName << "RangeClippingTransformation_apply(C" << color << "_RMIN"
             << rangeMin << "_RMAX" << rangeMax << ").png";

    Utils::createDirectories("Transformation");
    if (!cv::imwrite("Transformation/" + fileName.str(), img))
        throw std::runtime_error("Unable to write image: Transformation/"
                                 + fileName.str());
}

RUN_TESTS()
