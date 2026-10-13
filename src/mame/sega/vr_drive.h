// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    Sega 838-9081 (SJ25-0155-01) Motor PCB, Virtua Racing

***************************************************************************/

#ifndef MAME_SEGA_VR_DRIVE_H
#define MAME_SEGA_VR_DRIVE_H

#pragma once

#include "315_5296.h"

#include "cpu/z80/z80.h"
#include "machine/msm6253.h"


class sega_838_9081_device : public device_t
{
public:
	sega_838_9081_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

	// the wheel's potentiometer, read by the board itself
	auto wheel_callback() { return m_wheel_cb.bind(); }

	// link to the I/O board of each seat: commands in (its port E), replies out (its port D)
	void cmd_w(offs_t seat, u8 data);
	u8 reply_r(offs_t seat);

	// the machine's own I/O board, on the seat picked in the configuration
	void host_cmd_w(u8 data) { cmd_w(host_seat(), data); }
	u8 host_reply_r() { return reply_r(host_seat()); }

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_reset_after_children() override ATTR_COLD;
	virtual void device_add_mconfig(machine_config &config) override ATTR_COLD;
	virtual const tiny_rom_entry *device_rom_region() const override ATTR_COLD;
	virtual ioport_constructor device_input_ports() const override ATTR_COLD;

private:
	required_device<z80_device> m_cpu;
	required_device<sega_315_5296_device> m_io1;
	required_device<sega_315_5296_device> m_io2;
	required_ioport m_dsw1;
	required_ioport m_dsw2;
	required_ioport m_config;
	required_ioport m_wheel2;
	devcb_read8 m_wheel_cb;
	output_finder<2> m_motor;
	output_finder<4> m_digit;

	u8 m_cmd[2];
	u8 m_reply[2];
	u8 m_motor_byte[2];
	double m_wheel[2]; // the simulated wheels' positions, 0x80 the centre

	offs_t host_seat() const { return BIT(m_config->read(), 0); }
	bool simulated(int seat) const { return BIT(m_config->read(), 1 + seat); }
	void simulate(int seat);
	u8 wheel(int seat);

	void mem_map(address_map &map) ATTR_COLD;
	void io_map(address_map &map) ATTR_COLD;
	void timer_irq(device_t &device);
	ioport_value centre_r();
};

DECLARE_DEVICE_TYPE(SEGA_838_9081, sega_838_9081_device)

#endif // MAME_SEGA_VR_DRIVE_H
