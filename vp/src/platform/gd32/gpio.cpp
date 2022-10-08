#include "gpio.h"

static gpio::Pinstate getAFIO(gpio::PinNumber pin) {
	// TODO
	return gpio::Pinstate::IOF_SPI;
}

GPIO::GPIO(sc_core::sc_module_name, gpio::Port port) : port{port} {
	tsock.register_b_transport(this, &GPIO::transport);

	router
	    .add_register_bank({
	        {GPIO_CTL0_REG_ADDR, &gpio_ctl0},
	        {GPIO_CTL1_REG_ADDR, &gpio_ctl1},
	        {GPIO_ISTAT_REG_ADDR, &gpio_istat},
	        {GPIO_OCTL_REG_ADDR, &gpio_octl},
	        {GPIO_BOP_REG_ADDR, &gpio_bop},
	        {GPIO_BC_REG_ADDR, &gpio_bc},
	        {GPIO_LOCK_REG_ADDR, &gpio_lock},
	    })
	    .register_handler(this, &GPIO::register_access_callback);

	SC_METHOD(synchronousChange);
	sensitive << asyncEvent;
	dont_initialize();

	server.setupConnection(std::to_string(static_cast<int>(port)).c_str());
	server.registerOnChange(std::bind(&GPIO::asyncOnchange, this, std::placeholders::_1, std::placeholders::_2));
	serverThread = new std::thread(std::bind(&GpioServer::startAccepting, &server));
}

GPIO::~GPIO() {
	server.quit();
	if (serverThread) {
		if (serverThread->joinable()) {
			serverThread->join();
		}
		delete serverThread;
	}
}

void GPIO::asyncOnchange(gpio::PinNumber bit, gpio::Tristate val) {
	const auto state_prev = server.state.pins[bit];

	switch (val) {
		case gpio::Tristate::UNSET:
		case gpio::Tristate::LOW:
		case gpio::Tristate::HIGH:
			server.state.pins[bit] = toPinstate(val);
			break;
		default:
			std::cout << "[GPIO] Invalid pin value " << (int)val << " on pin " << bit << std::endl;
	}

	if (state_prev != server.state.pins[bit]) {
		asyncEvent.notify();
	}
}

void GPIO::synchronousChange() {
	// TODO - not complete and has some bugs
	gpio::State serverSnapshot = server.state;

	for (gpio::PinNumber i = 0; i < available_pins; i++) {
		const auto bitmask = 1l << i;
		uint32_t reg;

		if (i < 4) {
			reg = afio->afio_extiss0;
		} else if (i < 8) {
			reg = afio->afio_extiss1;
		} else if (i < 12) {
			reg = afio->afio_extiss2;
		} else {
			reg = afio->afio_extiss3;
		}

		const auto portNum = static_cast<int>(port) - static_cast<int>(gpio::Port::A);
		const auto afioMask = portNum << (4 * (i % 4));
		const auto afioEnable = ~(reg ^ afioMask);

		if ((exti->exti_inten & exti->exti_pd & bitmask) && afioEnable) {
			// TODO
			std::cout << "interrupt pending for pin " << static_cast<int>(i) << " port " << static_cast<int>(port)
			          << std::endl;
		}
	}
}

void GPIO::transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
	router.transport(trans, delay);
}

void GPIO::register_access_callback(const vp::map::register_access_t &r) {
	if (r.write) {
		int offset = 0;
		switch (r.addr) {
			case GPIO_CTL1_REG_ADDR:
				offset = 8;
				[[fallthrough]];
			case GPIO_CTL0_REG_ADDR: {
				for (gpio::PinNumber i = 0; i < 8; i++) {
					auto output_en = (((r.nv >> (4 * i)) & 0b11) > 0);
					auto afio_en = (((r.nv >> ((4 * i) + 2)) & 0b11) > 1);
					if (output_en & afio_en) {
						const auto afio = getAFIO(i + offset);
						if (afio == gpio::Pinstate::UNSET)
							std::cerr << "[GPIO] Set invalid afio to pin " << (int)i << std::endl;
						else
							server.state.pins[i + offset] = afio;
					}
				}
				break;
			}
			case GPIO_BOP_REG_ADDR: {
				const uint16_t set = (uint16_t)r.nv;
				const uint16_t clear = (uint16_t)(r.nv >> 16);

				uint16_t output_en = 0;
				for (int i = 0; i < 8; i++) {
					auto md_mask = 0b11 << (4 * i);
					output_en |= ((gpio_ctl0 & md_mask) > 0) << i;
					output_en |= ((gpio_ctl1 & md_mask) > 0) << (i + 8);
				}

				gpio_octl &= ~(clear & output_en);
				gpio_octl |= (set & output_en);

				const uint16_t changed_bits = (set | clear) & output_en;

				for (gpio::PinNumber i = 0; i < available_pins; i++) {
					const auto bitoffs = (1l << i);
					if (bitoffs & changed_bits) {
						if (set & bitoffs)
							server.state.pins[i] = gpio::Pinstate::HIGH;
						else if (clear & bitoffs)
							server.state.pins[i] = gpio::Pinstate::LOW;
						server.pushPin(i, gpio::toTristate(server.state.pins[i]));
					}
				}
				break;
			}
			case GPIO_BC_REG_ADDR: {
				uint16_t clear = (uint16_t)r.nv;

				uint16_t output_en = 0;
				for (int i = 0; i < 8; i++) {
					auto md_mask = 0b11 << (4 * i);
					output_en |= ((gpio_ctl0 & md_mask) > 0) << i;
					output_en |= ((gpio_ctl1 & md_mask) > 0) << (i + 8);
				}

				gpio_octl &= ~(clear & output_en);

				for (gpio::PinNumber i = 0; i < available_pins; i++) {
					const auto bitoffs = (1l << i);
					if (bitoffs & clear & output_en) {
						server.state.pins[i] = gpio::Pinstate::LOW;
						server.pushPin(i, gpio::toTristate(server.state.pins[i]));
					}
				}
				break;
			}
			case GPIO_OCTL_REG_ADDR: {
				for (gpio::PinNumber i = 0; i < available_pins; i++) {
					const auto bitoffs = (1l << i);
					server.state.pins[i] = (uint16_t)r.nv & bitoffs ? gpio::Pinstate::HIGH : gpio::Pinstate::LOW;
					server.pushPin(i, gpio::toTristate(server.state.pins[i]));
				}
				break;
			}

			default:
				break;
		}
	}
	r.fn();
}

SpiWriteFunction GPIO::getSPIwriteFunction(gpio::PinNumber cs) {
	return std::bind(&GpioServer::pushSPI, &server, cs, std::placeholders::_1);
}
