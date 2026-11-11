#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void bsc_init(void);
void bsc_reset(bool hard);
void bsc_term(void);


void cpg_init(void);
void cpg_reset(void);
void cpg_term(void);

void dmac_init(void);
void dmac_reset(void);
void dmac_term(void);

void rtc_init(void);
void rtc_reset(void);
void rtc_term(void);

void intc_init(void);
void intc_reset(void);
void intc_term(void);

void serial_init(void);
void serial_reset(void);
void serial_term(void);

#ifdef __cplusplus
}
#include "hw/serial_port.h"
/* The device plugged into the SCIF, if any (serial.cpp): NULL unplugs it.
 * scif_update_status() is for a device that has something new to send. */
void scif_set_pipe(SerialPort::Pipe *pipe);
SerialPort::Pipe *scif_get_pipe(void);
void scif_update_status(void);
/* what a device's port has in a state since V24: see serial.cpp */
void scif_serialize_v24(void **data, unsigned int *total_size);
bool scif_unserialize_v24(void **data, unsigned int *total_size);
void scif_state_before_v24(void);

/* The same, the way the peripherals' code has it */
class SCIFSerialPort : public SerialPort
{
public:
	void setPipe(Pipe *pipe) override;
	Pipe *getPipe() const { return scif_get_pipe(); }
	void updateStatus() override;
	static SCIFSerialPort& Instance();
};
extern "C" {
#endif

void ubc_init(void);
void ubc_reset(void);
void ubc_term(void);

void tmu_init(void);
void tmu_reset(bool hard);
void tmu_term(void);
/* after a state has been loaded: see tmu.cpp */
void tmu_state_loaded(void);

void ccn_init(void);
void ccn_reset(bool hard);
void ccn_term(void);

void MMU_init(void);
void MMU_reset(void);
void MMU_term(void);

#ifdef __cplusplus
}
#endif
