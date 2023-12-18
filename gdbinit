target extended-remote :3333

define bld
make -C build
load
mri
end

define mri
monitor reset init
end
