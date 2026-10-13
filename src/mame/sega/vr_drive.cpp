// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    Sega 838-9081 (SJ25-0155-01) Motor PCB, Virtua Racing

    One board for both seats of a Virtua Racing Twin, each seat a stack of
    Model 1 boards with its own I/O board: CN1, CN2 to player 1's I/O
    board, CN3, CN4 to player 2's, CN11 and CN12 to their motors and
    clutches. Z80 at 8 MHz / 2, EPR-15111 (27C512), 8 KB of SRAM, two
    315-5296, an MSM6253 for the wheels' potentiometers, two MB3759 driving
    the clutches, four 7-segment digits, two banks of DIP switches.

    The game sends a command byte on port E of its I/O board (when its
    CABINET TYPE is not STANDARD) and reads the reply on port D.

    As the program of EPR-15111 uses it:
    - 315-5296 #1: port H player 1's command, port D its reply; port A
      player 2's command, port B its reply; port F DSW1, port G DSW2.
    - 315-5296 #2: port G player 1's motor, port F player 2's (bits 5-0
      the clutch's power, bit 7 low one way, bit 6 low the other, all
      complemented); ports C D player 1's digits, A B player 2's.
    - MSM6253: channel 0 player 1's wheel, 1 player 2's (2 and 3 are only
      shown by the self-tests).
    - The interrupt (IM 1) runs the links, the ADC and the counters; the
      program counts its times in interrupts (1000 a calibration step, 100
      before the motor turns the other way). What raises it on the board is
      not known: here a timer, at a guessed rate.

    The machine's own I/O board is on player 1's link, or player 2's (the
    configuration): a Twin is two machines on one board, the other link can
    be fed from outside (a script), and its wheel too. The wheels'
    potentiometers are the game's wheel on the machine's seat and the other
    seat's wheel input on the other, or simulated panels, for the
    calibration at power-up to find something.

    TODO: the interrupt's source and rate.

***************************************************************************/

#include "emu.h"
#include "vr_drive.h"

#define LOG_PORTS (1U << 1)

#define VERBOSE (LOG_PORTS)
#include "logmacro.h"

#include <algorithm>
#include <cmath>

namespace {

// [guess] the board's interrupt rate. Its handler takes about 1800 of the
// Z80's cycles: at 2 kHz the tasks would get no time at all.
constexpr u32 IRQ_HZ = 1000;

// The simulated panels, in the clutch's power: a friction, a spring towards
// the centre (power per unit of the ADC away from it), the motor's torque the
// clutch's power; the standing motor, its clutch engaged, brakes. The wheel
// moves at SIM_SPEED units of the ADC an interrupt per unit of power beyond
// what holds it. Not measured: enough for the calibration to find a power for
// each place, 3 at the centre, 11 or so at the ends, and for the spring to
// bring the wheel back into the centre zone once the clutch lets it go.
constexpr double SIM_FRICTION = 1.0;
constexpr double SIM_SPRING = 1.0 / 8;
constexpr double SIM_SPEED = 0.05;
constexpr double SIM_MIN = 0x10, SIM_MAX = 0xf0; // the stops

}


DEFINE_DEVICE_TYPE(SEGA_838_9081, sega_838_9081_device, "sega_838_9081", "Sega 838-9081 Motor PCB (Virtua Racing)")

sega_838_9081_device::sega_838_9081_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock) :
	device_t(mconfig, SEGA_838_9081, tag, owner, clock),
	m_cpu(*this, "drivecpu"),
	m_io1(*this, "driveio1"),
	m_io2(*this, "driveio2"),
	m_dsw1(*this, "DSW1"),
	m_dsw2(*this, "DSW2"),
	m_config(*this, "CONFIG"),
	m_wheel2(*this, "WHEEL2"),
	m_wheel_cb(*this, 0x80),
	m_motor(*this, "drive_motor%u", 1U),
	m_digit(*this, "drive_digit%u", 0U)
{
}

ROM_START( sega_838_9081 )
	ROM_REGION( 0x10000, "drivecpu", 0 )
	ROM_LOAD( "epr-15111.12", 0x0000, 0x10000, CRC(45c02763) SHA1(9528dc78162f3216c6c5179fdc41bf7bea3cea49) )
ROM_END

const tiny_rom_entry *sega_838_9081_device::device_rom_region() const
{
	return ROM_NAME(sega_838_9081);
}

// Switch 4, the power-on calibration, defaults to off (ON): in the emulator
// the motor does not move the wheel, so a calibration would fail and leave
// the side without torque. Switches 7 and 8 of DSW1 pick the self-tests.
static INPUT_PORTS_START( sega_838_9081 )
	PORT_START("DSW1")
	PORT_DIPNAME( 0x07, 0x07, "Feedback Weight PL1" ) PORT_DIPLOCATION("DSW1:1,2,3")
	PORT_DIPSETTING(    0x07, "0 (Light)" )
	PORT_DIPSETTING(    0x06, "1" )
	PORT_DIPSETTING(    0x05, "2" )
	PORT_DIPSETTING(    0x04, "3" )
	PORT_DIPSETTING(    0x03, "4" )
	PORT_DIPSETTING(    0x02, "5" )
	PORT_DIPSETTING(    0x01, "6" )
	PORT_DIPSETTING(    0x00, "7 (Heavy)" )
	PORT_DIPNAME( 0x08, 0x00, "Power On Check PL1" ) PORT_DIPLOCATION("DSW1:4")
	PORT_DIPSETTING(    0x08, DEF_STR( Yes ) )
	PORT_DIPSETTING(    0x00, DEF_STR( No ) )
	PORT_DIPUNUSED_DIPLOC( 0x10, 0x10, "DSW1:5" )
	PORT_DIPUNUSED_DIPLOC( 0x20, 0x20, "DSW1:6" )
	PORT_DIPNAME( 0xc0, 0xc0, "Mode" ) PORT_DIPLOCATION("DSW1:7,8")
	PORT_DIPSETTING(    0xc0, "Game" )
	PORT_DIPSETTING(    0x80, "Game (7 ON)" )
	PORT_DIPSETTING(    0x40, "Loop Test" )
	PORT_DIPSETTING(    0x00, "Bench Test" )

	PORT_START("DSW2")
	PORT_DIPNAME( 0x07, 0x07, "Feedback Weight PL2" ) PORT_DIPLOCATION("DSW2:1,2,3")
	PORT_DIPSETTING(    0x07, "0 (Light)" )
	PORT_DIPSETTING(    0x06, "1" )
	PORT_DIPSETTING(    0x05, "2" )
	PORT_DIPSETTING(    0x04, "3" )
	PORT_DIPSETTING(    0x03, "4" )
	PORT_DIPSETTING(    0x02, "5" )
	PORT_DIPSETTING(    0x01, "6" )
	PORT_DIPSETTING(    0x00, "7 (Heavy)" )
	PORT_DIPNAME( 0x08, 0x00, "Power On Check PL2" ) PORT_DIPLOCATION("DSW2:4")
	PORT_DIPSETTING(    0x08, DEF_STR( Yes ) )
	PORT_DIPSETTING(    0x00, DEF_STR( No ) )
	PORT_DIPUNUSED_DIPLOC( 0x10, 0x10, "DSW2:5" )
	PORT_DIPUNUSED_DIPLOC( 0x20, 0x20, "DSW2:6" )
	PORT_DIPNAME( 0xc0, 0xc0, "7-SEG LED" ) PORT_DIPLOCATION("DSW2:7,8")
	PORT_DIPSETTING(    0xc0, "V.R. Value 00-80-FF" )
	PORT_DIPSETTING(    0x80, "Clutch Value" )
	PORT_DIPSETTING(    0x40, "Data from the I/O Board" )
	PORT_DIPSETTING(    0x00, "Data to the I/O Board" )

	PORT_START("CONFIG")
	PORT_CONFNAME( 0x01, 0x00, "Drive Board Seat" )
	PORT_CONFSETTING(    0x00, "Player 1 (Twin left, or a single cabinet)" )
	PORT_CONFSETTING(    0x01, "Player 2 (Twin right)" )
	PORT_CONFNAME( 0x06, 0x00, "Drive Board Wheels" )
	PORT_CONFSETTING(    0x00, "The game's wheel" )
	PORT_CONFSETTING(    0x06, "Simulated panels" )
	PORT_CONFSETTING(    0x02, "Player 1's simulated, player 2's the input" )
	PORT_CONFSETTING(    0x04, "Player 1's the input, player 2's simulated" )

	// the other seat's wheel: on a Twin, the other machine's
	PORT_START("WHEEL2")
	PORT_BIT( 0xff, 0x80, IPT_PADDLE ) PORT_PLAYER(2) PORT_SENSITIVITY(100) PORT_KEYDELTA(3) PORT_NAME("Other Seat Wheel")
INPUT_PORTS_END

ioport_constructor sega_838_9081_device::device_input_ports() const
{
	return INPUT_PORTS_NAME(sega_838_9081);
}

void sega_838_9081_device::mem_map(address_map &map)
{
	map(0x0000, 0x7fff).rom().region("drivecpu", 0);
	map(0xe000, 0xffff).ram();
}

void sega_838_9081_device::io_map(address_map &map)
{
	map.global_mask(0xff);
	map(0x00, 0x00).nopw(); // watchdog
	map(0x20, 0x2f).rw(m_io1, FUNC(sega_315_5296_device::read), FUNC(sega_315_5296_device::write));
	map(0x40, 0x4f).rw(m_io2, FUNC(sega_315_5296_device::read), FUNC(sega_315_5296_device::write));
	map(0x80, 0x83).rw("driveadc", FUNC(msm6253_device::d0_r), FUNC(msm6253_device::address_w));
}

void sega_838_9081_device::device_add_mconfig(machine_config &config)
{
	Z80(config, m_cpu, 8_MHz_XTAL / 2);
	m_cpu->set_addrmap(AS_PROGRAM, &sega_838_9081_device::mem_map);
	m_cpu->set_addrmap(AS_IO, &sega_838_9081_device::io_map);
	m_cpu->set_periodic_int(FUNC(sega_838_9081_device::timer_irq), attotime::from_hz(IRQ_HZ));

	SEGA_315_5296(config, m_io1);
	m_io1->in_ph_callback().set([this] () { return m_cmd[0]; });
	m_io1->in_pa_callback().set([this] () { return m_cmd[1]; });
	m_io1->out_pd_callback().set([this] (u8 data) { m_reply[0] = data; });
	m_io1->out_pb_callback().set([this] (u8 data) { m_reply[1] = data; });
	m_io1->in_pf_callback().set_ioport("DSW1");
	m_io1->in_pg_callback().set_ioport("DSW2");

	SEGA_315_5296(config, m_io2);
	m_io2->out_pg_callback().set([this] (u8 data) { m_motor[0] = m_motor_byte[0] = data; });
	m_io2->out_pf_callback().set([this] (u8 data) { m_motor[1] = m_motor_byte[1] = data; });
	m_io2->out_pc_callback().set([this] (u8 data) { m_digit[0] = data; });
	m_io2->out_pd_callback().set([this] (u8 data) { m_digit[1] = data; });
	m_io2->out_pa_callback().set([this] (u8 data) { m_digit[2] = data; });
	m_io2->out_pb_callback().set([this] (u8 data) { m_digit[3] = data; });

	msm6253_device &adc(MSM6253(config, "driveadc"));
	adc.set_input_cb<0>(NAME([this] () -> ioport_value { return wheel(0); }));
	adc.set_input_cb<1>(NAME([this] () -> ioport_value { return wheel(1); }));
	adc.set_input_cb<2>(FUNC(sega_838_9081_device::centre_r));
	adc.set_input_cb<3>(FUNC(sega_838_9081_device::centre_r));
}

void sega_838_9081_device::device_start()
{
	std::fill(std::begin(m_cmd), std::end(m_cmd), 0);
	std::fill(std::begin(m_reply), std::end(m_reply), 0xff);
	std::fill(std::begin(m_motor_byte), std::end(m_motor_byte), 0xff);
	std::fill(std::begin(m_wheel), std::end(m_wheel), 128.0);

	save_item(NAME(m_cmd));
	save_item(NAME(m_reply));
	save_item(NAME(m_motor_byte));
	save_item(NAME(m_wheel));
}

void sega_838_9081_device::device_reset()
{
}

// The 315-5296s output 0 at reset, their ports still inputs: on the board
// these lines are left high (all off) until the program sets the ports as
// outputs, having written FF to them first.
void sega_838_9081_device::device_reset_after_children()
{
	for (int i = 0; i < 2; i++)
	{
		m_reply[i] = 0xff;
		m_motor[i] = m_motor_byte[i] = 0xff;
		m_wheel[i] = 128.0;
	}
	for (auto &digit : m_digit)
		digit = 0xff;
}

void sega_838_9081_device::timer_irq(device_t &device)
{
	for (int seat = 0; seat < 2; seat++)
		if (simulated(seat))
			simulate(seat);
	m_cpu->set_input_line(0, HOLD_LINE);
}

void sega_838_9081_device::simulate(int seat)
{
	u8 const byte = m_motor_byte[seat];
	double const power = ~byte & 0x3f;
	int const way = !BIT(byte, 7) ? -1 : !BIT(byte, 6) ? 1 : 0; // bit 7 low lowers the position
	double const friction = SIM_FRICTION + (way ? 0.0 : power);
	double const net = way * power - (m_wheel[seat] - 128.0) * SIM_SPRING;
	if (std::abs(net) <= friction)
		return;
	double const pos = m_wheel[seat] + (net - (net > 0 ? friction : -friction)) * SIM_SPEED;
	m_wheel[seat] = std::clamp(pos, SIM_MIN, SIM_MAX);
}

u8 sega_838_9081_device::wheel(int seat)
{
	if (simulated(seat))
		return u8(m_wheel[seat] + 0.5);
	return (seat == host_seat()) ? m_wheel_cb() : m_wheel2->read();
}

ioport_value sega_838_9081_device::centre_r()
{
	return 0x80;
}

void sega_838_9081_device::cmd_w(offs_t seat, u8 data)
{
	seat &= 1;
	if (m_cmd[seat] != data)
		LOGMASKED(LOG_PORTS, "seat %d command %02x\n", seat, data);
	m_cmd[seat] = data;
}

u8 sega_838_9081_device::reply_r(offs_t seat)
{
	return m_reply[seat & 1];
}
