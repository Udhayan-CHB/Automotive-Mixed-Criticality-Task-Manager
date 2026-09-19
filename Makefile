COMPONENTS := VehicleIO ResourceMonitor SafetyMonitor SafetyTask ControlTask Infotainment FaultInjector TraceViewer

.PHONY: all clean rebuild

all:
	@for dir in $(COMPONENTS); do \
		echo "[BUILD] $$dir"; \
		$(MAKE) -C $$dir all || exit $$?; \
	done

clean:
	@for dir in $(COMPONENTS); do \
		$(MAKE) -C $$dir clean; \
	done

rebuild: clean all
