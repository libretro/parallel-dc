// The motion vector pass (mv.vert): the vector, as it is

layout (location = 0) in vec2 motion;

layout (location = 0) out vec2 fragmentColor;

void main()
{
	fragmentColor = motion;
}
