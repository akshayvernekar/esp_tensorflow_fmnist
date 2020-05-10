
f = open("normalisation_lookup.h", "a")
f.write("static const __flash uint8_t lookup[256] =")
f.write("{\n")
i = 0
while i < 255 :
	f.write("%f ,\t"%(i/255.0))
	f.write("%f ,\t"%((i+1)/255.0))
	f.write("%f ,\t"%((i+2)/255.0))
	f.write("%f ,\t"%((i+3)/255.0))
	f.write("%f ,\t"%((i+4)/255.0))
	f.write("%f ,\t"%((i+5)/255.0))
	f.write("%f ,\t"%((i+6)/255.0))
	f.write("%f ,\t"%((i+7)/255.0))
	f.write("\n")
	i = i + 8;
f.write("}\n")
f.close()