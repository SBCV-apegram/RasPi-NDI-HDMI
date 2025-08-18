#include <chrono>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/stat.h>

#include "core/rpicam_encoder.hpp"
#include "ndi_output.hpp"
#include <libconfig.h++>

using namespace std::placeholders;
bool exit_loop = false;
libconfig::Config cfg;
static int signal_received;

static void sigint_handler(int)
{
	exit_loop = true;
}

int loadConfig()
{
	try
	{
		cfg.readFile("/etc/raspindi.conf");
	}
	catch(const libconfig::FileIOException &fioex)
	{
		std::cerr << "Could not open config file /etc/raspindi.conf"
			<< std::endl;
		return(EXIT_FAILURE);
	}
	catch(const libconfig::ParseException &pex)
	{
		std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine()
              << " - " << pex.getError() << std::endl;
    	return(EXIT_FAILURE);
	}
	return 0;
}

int _getValue(std::string parameter, int defaultValue, int min, int max)
{
    try
    {
        int value = cfg.lookup(parameter);
        if (value > max)
        {
            std::cerr << "Invalid value for " << parameter << ": " << value << std::endl;
            return 100;
        }
        if (value < 0)
        {
            std::cerr << "Invalid value for " << parameter << ": " << value << std::endl;
            return 0;
        }
        return value;
    } catch (libconfig::SettingNotFoundException)
    {
        return defaultValue;
    }
}

int _getValue(std::string parameter, int defaultValue)
{
	try
    {
        int value = cfg.lookup(parameter);
        return value;
    } catch (libconfig::SettingNotFoundException)
    {
        return defaultValue;
    }
}

float _getValue(std::string parameter, float defaultValue)
{
	try
    {
        float value = cfg.lookup(parameter);
        return value;
    } catch (libconfig::SettingNotFoundException)
    {
        return defaultValue;
    }
}

std::string _getValue(std::string parameter, std::string defaultValue)
{
	try
    {
        std::string value = cfg.lookup(parameter);
        return value;
    } catch (libconfig::SettingNotFoundException)
    {
        return defaultValue;
    }
}

void mirrored_rotation(VideoOptions *options)
{
	std::string value = _getValue("mirror", "none");
	libcamera::Transform transform = libcamera::Transform::Identity;
	bool hflip = false;
	bool vflip = false;
	if (value == "horizontal")
	{
		hflip = true;
	}
	if (value == "vertical")
	{
		vflip = true;
	}
	if (value == "both")
	{
		hflip = true;
		vflip = true;
	}
	if (hflip)
		transform = libcamera::Transform::HFlip * transform;
	if (vflip)
		transform = libcamera::Transform::VFlip * transform;
	
	bool ok;
	libcamera::Transform rotation = libcamera::transformFromRotation(_getValue("rotation", 0), &ok);
	if (!ok)
		throw std::runtime_error("illegal rotation value");
	transform = rotation * transform;
	options->Set().transform = transform;
}

static void default_signal_handler(int signal_number)
{
	signal_received = signal_number;
	LOG(1, "Received signal " << signal_number);
}

static int get_key_or_signal(VideoOptions const *options, pollfd p[1])
{
	int key = 0;
	if (signal_received == SIGINT)
		return 'x';
	if (options->Get().keypress)
	{
		poll(p, 1, 0);
		if (p[0].revents & POLLIN)
		{
			char *user_string = nullptr;
			size_t len;
			[[maybe_unused]] size_t r = getline(&user_string, &len, stdin);
			key = user_string[0];
		}
	}
	if (options->Get().signal)
	{
		if (signal_received == SIGUSR1)
			key = '\n';
		else if ((signal_received == SIGUSR2) || (signal_received == SIGPIPE))
			key = 'x';
		signal_received = 0;
	}
	return key;
}

static int get_colourspace_flags(std::string const &codec)
{
	if (codec == "mjpeg" || codec == "yuv420")
		return RPiCamEncoder::FLAG_VIDEO_JPEG_COLOURSPACE;
	else
		return RPiCamEncoder::FLAG_VIDEO_NONE;
}

// The main even loop for the application.

static void event_loop(RPiCamEncoder &app)
{
	VideoOptions const *options = app.GetOptions();
	//std::unique_ptr<Output> output = std::unique_ptr<Output>(Output::Create(options));
	std::unique_ptr<Output> output = std::unique_ptr<Output>(new NdiOutput(options, _getValue("neopixel_path", "/tmp/neopixel.state")));
	app.SetEncodeOutputReadyCallback(std::bind(&Output::OutputReady, output.get(), _1, _2, _3, _4));
	app.SetMetadataReadyCallback(std::bind(&Output::MetadataReady, output.get(), _1));

	app.OpenCamera();
	app.ConfigureVideo(get_colourspace_flags(options->Get().codec));
	app.StartEncoder();
	app.StartCamera();
	auto start_time = std::chrono::high_resolution_clock::now();

	// Monitoring for keypresses and signals.
	signal(SIGUSR1, default_signal_handler);
	signal(SIGUSR2, default_signal_handler);
	signal(SIGINT, default_signal_handler);
	// SIGPIPE gets raised when trying to write to an already closed socket. This can happen, when
	// you're using TCP to stream to VLC and the user presses the stop button in VLC. Catching the
	// signal to be able to react on it, otherwise the app terminates.
	signal(SIGPIPE, default_signal_handler);
	pollfd p[1] = { { STDIN_FILENO, POLLIN, 0 } };

	for (unsigned int count = 0; ; count++)
	{
		RPiCamEncoder::Msg msg = app.Wait();
		if (msg.type == RPiCamApp::MsgType::Timeout)
		{
			LOG_ERROR("ERROR: Device timeout detected, attempting a restart!!!");
			app.StopCamera();
			app.StartCamera();
			continue;
		}
		if (msg.type == RPiCamEncoder::MsgType::Quit)
			return;
		else if (msg.type != RPiCamEncoder::MsgType::RequestComplete)
			throw std::runtime_error("unrecognised message!");
		int key = get_key_or_signal(options, p);
		if (key == '\n')
			output->Signal();

		LOG(2, "Viewfinder frame " << count);
		auto now = std::chrono::high_resolution_clock::now();
		bool timeout = !options->Get().frames && options->Get().timeout &&
					   ((now - start_time) > options->Get().timeout.value);
		bool frameout = options->Get().frames && count >= options->Get().frames;
		if (timeout || frameout || key == 'x' || key == 'X')
		{
			if (timeout)
				LOG(1, "Halting: reached timeout of " << options->Get().timeout.get<std::chrono::milliseconds>()
													  << " milliseconds.");
			app.StopCamera(); // stop complains if encoder very slow to close
			app.StopEncoder();
			return;
		}
		CompletedRequestPtr &completed_request = std::get<CompletedRequestPtr>(msg.payload);
		if (!app.EncodeBuffer(completed_request, app.VideoStream()))
		{
			// Keep advancing our "start time" if we're still waiting to start recording (e.g.
			// waiting for synchronisation with another camera).
			start_time = now;
			count = 0; // reset the "frames encoded" counter too
		}
		app.ShowPreview(completed_request, app.VideoStream());
	}

/*	VideoOptions const *options = app.GetOptions();
	std::unique_ptr<Output> output = std::unique_ptr<Output>(new NdiOutput(options, _getValue("neopixel_path", "/tmp/neopixel.state")));
	app.SetEncodeOutputReadyCallback(std::bind(&Output::OutputReady, output.get(), _1, _2, _3, _4));


	app.OpenCamera();
	app.ConfigureVideo(RPiCamEncoder::FLAG_VIDEO_JPEG_COLOURSPACE);
	app.StartEncoder();
	app.StartCamera();
	while (!exit_loop)
	{
		RPiCamEncoder::Msg msg = app.Wait();
		if (msg.type == RPiCamEncoder::MsgType::Quit)
			return;
		else if (msg.type != RPiCamEncoder::MsgType::RequestComplete)
			throw std::runtime_error("unrecognised message!");

		CompletedRequestPtr &completed_request = std::get<CompletedRequestPtr>(msg.payload);
		app.EncodeBuffer(completed_request, app.VideoStream());
	}
*/}

int main(int argc, char *argv[])
{
	try
	{
		RPiCamEncoder app;
		VideoOptions *options = app.GetOptions();
		loadConfig();
		options->Set().codec = "YUV420";
		options->Set().verbose = false;
		options->Set().nopreview = true;
		options->Set().denoise = "off";
		// Set flip
		options->Set().width = _getValue("width", 1280);
		options->Set().height = _getValue("height", 720);
		options->Set().framerate = _getValue("framerate", 25);
		options->Set().awb = _getValue("awb", "auto");
		options->Set().awb_gain_b = _getValue("b_gain", 0.0f);
		options->Set().awb_gain_r = _getValue("r_gain", 0.0f);
		options->Set().saturation = _getValue("saturation", 1);
		options->Set().sharpness = _getValue("sharpness", 1);
		options->Set().contrast = _getValue("contrast", 1);
		options->Set().brightness = ((_getValue("brightness", 50) / 50) - 1);
		options->Set().exposure = _getValue("exposuremode", "auto");
		options->Set().metering = _getValue("meteringmode", "average");
		mirrored_rotation(options);
	//	options->Print();
		event_loop(app);
	}
	catch (std::exception const &e)
	{
		std::cerr << "ERROR: *** " << e.what() << " ***" << std::endl;
		return -1;
	}
	return 0;
}