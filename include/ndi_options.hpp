/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * still_video.hpp - video capture program options
 */

#pragma once

#include <cstdio>

#include <string>

#include "core/video_options.hpp"

struct VideoOptions : public Options
{
	NDIOptions() : VideoOptions()
	{
		using namespace boost::program_options;
		// Generally we shall use zero or empty values to avoid over-writing the
		// codec's default behaviour.  = "";
		// clang-format off 
		options_->add_options()
			("neopixel_path", value<std::string>(&v_->neopixel_path_)->default_value("/tmp/neopixel.state"),
			 "Set the location for the neopixel state.")
		;
		// clang-format on
	}

	virtual bool Parse(int argc, char *argv[]) override
	{
		if (Options::Parse(argc, argv) == false)
			return false;

		return v_->ParseVideo();
	}

	virtual void Print() const override
	{
		Options::Print();
		v_->PrintVideo();
	}
};
