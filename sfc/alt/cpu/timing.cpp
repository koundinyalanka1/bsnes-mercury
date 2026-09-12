#ifdef CPU_CPP

void CPU::queue_event(unsigned id) {
  switch(id) {
    case QueueEvent::DramRefresh: return add_clocks(40);
    case QueueEvent::HdmaRun: return hdma_run();
  }
}

void CPU::last_cycle() {
  if(status.irq_lock) {
    status.irq_lock = false;
    return;
  }

  if(status.nmi_transition) {
    regs.wai = false;
    status.nmi_transition = false;
    status.nmi_pending = true;
#ifdef SFC_LAGFIX
    if (!status.frame_event_performed) {
      scheduler.exit(Scheduler::ExitReason::FrameEvent);
    }
    status.frame_event_performed = true;
#endif
  }

  if(status.irq_transition || regs.irq) {
    regs.wai = false;
    status.irq_transition = false;
    status.irq_pending = !regs.p.i;
  }
}

//Both values below depend only on registers, so they are rebuilt on write instead
//of on every memory access. frame_clocks still needs field() added at use, because
//the field flips mid-frame.
void CPU::update_irq_time() {
  irq_time = status.vtime * 1364 + status.htime * 4;
  irq_htime4 = status.htime * 4;
}

void CPU::update_frame_clocks() {
  frame_clocks = (system.region() == System::Region::NTSC ? 262 : 312) * 1364;
}

//Runs on every CPU memory access, so it is inline and call-free.
//
//`(target - now) < clocks` on unsigned values is exactly the original
//`now <= target && now + clocks > target`, for every input rather than only the
//reachable ones:
//  now >  target: the subtraction wraps to at least 2^32 - 357368, which exceeds
//                 any clocks value, so both forms are false.
//  now <= target: target - now cannot overflow, so the test is clocks > target - now,
//                 which is the second conjunct, and the first is already true.
void CPU::poll_irq(unsigned clocks) {
  if(status.hirq_enabled) {
    unsigned now, target;
    if(status.virq_enabled) {
      now = vcounter() * 1364 + hcounter();
      target = irq_time;
      if(now > target) target += frame_clocks + (field() ? 1364 : 0);
    } else {
      now = hcounter();
      target = irq_htime4;
      if(now > target) target += 1364;
    }
    bool valid = (target - now) < clocks;
    if(!status.irq_valid && valid) status.irq_line = true;
    status.irq_valid = valid;
    if(status.irq_line) status.irq_transition = true;
  } else if(status.virq_enabled) {
    bool valid = vcounter() == status.vtime;
    if(!status.irq_valid && valid) status.irq_line = true;
    status.irq_valid = valid;
    if(status.irq_line) status.irq_transition = true;
  } else {
    status.irq_valid = false;
  }
}

void CPU::add_clocks(unsigned clocks) {
  poll_irq(clocks);

  tick(clocks);
  queue.tick(clocks);
  step(clocks);
}

void CPU::scanline() {
  //synchronize_smp() settles the shared debt. The ports then need their own switch
  //only in the case step() skipped it, which is exactly when there was debt to
  //settle and the ports are passive. Switching unconditionally would move when an
  //active controller -- a light gun polling the raster -- runs.
  bool settled = pending_clocks != 0;
  synchronize_smp();
  if(settled && input.ports_passive) synchronize_controllers();
  synchronize_ppu();
  synchronize_coprocessors();
  system.scanline(status.frame_event_performed);

  if(vcounter() == 0) hdma_init();

  queue.enqueue(534, QueueEvent::DramRefresh);

  if(vcounter() <= (ppu.overscan() == false ? 224 : 239)) {
    queue.enqueue(1104 + 8, QueueEvent::HdmaRun);
  }

  bool nmi_valid = status.nmi_valid;
  status.nmi_valid = vcounter() >= (ppu.overscan() == false ? 225 : 240);
  if(!nmi_valid && status.nmi_valid) {
    status.nmi_line = true;
    if(status.nmi_enabled) status.nmi_transition = true;
  } else if(nmi_valid && !status.nmi_valid) {
    status.nmi_line = false;
    status.frame_event_performed = false;
  }

  if(status.auto_joypad_poll_enabled && vcounter() == (ppu.overscan() == false ? 227 : 242)) {
    run_auto_joypad_poll();
  }
}

void CPU::run_auto_joypad_poll() {
  input.port1->latch(1);
  input.port2->latch(1);
  input.port1->latch(0);
  input.port2->latch(0);

  uint16 joy1 = 0, joy2 = 0, joy3 = 0, joy4 = 0;
  for(unsigned i = 0; i < 16; i++) {
    uint8 port0 = input.port1->data();
    uint8 port1 = input.port2->data();

    joy1 |= (port0 & 1) ? (0x8000 >> i) : 0;
    joy2 |= (port1 & 1) ? (0x8000 >> i) : 0;
    joy3 |= (port0 & 2) ? (0x8000 >> i) : 0;
    joy4 |= (port1 & 2) ? (0x8000 >> i) : 0;
  }

  status.joy1l = joy1;
  status.joy1h = joy1 >> 8;

  status.joy2l = joy2;
  status.joy2h = joy2 >> 8;

  status.joy3l = joy3;
  status.joy3h = joy3 >> 8;

  status.joy4l = joy4;
  status.joy4h = joy4 >> 8;
}

#endif
