/* Porte de X1Rus src/testbga.cpp (starfield). Lógica preservada;
 * S3D* trocado pelas chamadas OpenGL equivalentes. */
#include "pumpy.h"
#include "testbga.h"
#include <stdlib.h>
#include <time.h>

///// Sobre o plano de fundo
#define ZMAX        800
#define ZMIN        -100
#define STAR_COUNT  200

#define random(x) (rand() % x)

typedef struct
{
	int PosX, PosY;
	int CurZ;
	int OldX, OldY;
	int Type;
	float Color[3];
} STAR;

static STAR Star[STAR_COUNT];
static DWORD  g_starLastMs;   /* port: relógio do passo de 1/60 s (DrawStar) */
static double g_starAcc;

static int CenterX = 0, CenterY = 0;
static int StepZ;

static void InitStar( int no )
{
	Star[no].PosX  = random( 800 ) - 400;
	Star[no].PosY  = random( 600 ) - 300;
	Star[no].CurZ  = 100 + random( ZMAX - 100 );
	Star[no].Color[0] = random(255) / 255.0f;
	Star[no].Color[1] = random(255) / 255.0f;
	Star[no].Color[2] = random(255) / 255.0f;
	Star[no].OldX = CenterX + Star[no].PosX;
	Star[no].OldY = CenterY + Star[no].PosY;
	Star[no].Type = random(5);
}

/* Type != 0: 1 ponto; Type == 0: cruz de 5 pontos (como no original) */
static void PlotStar( int x, int y, int type )
{
	glBegin(GL_POINTS);
		glVertex2i( x, y );
		if (!type)
		{
			glVertex2i( x-1, y );
			glVertex2i( x+1, y );
			glVertex2i( x, y+1 );
			glVertex2i( x, y-1 );
		}
	glEnd();
}

/* Port: o original anda um passo por quadro (60 Hz). draw = 0 só avança
 * (passos de recuperação), sem desenhar. */
static void MoveStars( int draw )
{
	int i, NewX, NewY;
	long scale;

	if(random(2))
	{
		if(random(2)) CenterX++;
		else CenterX--;
		if(random(2)) CenterY++;
		else CenterY--;
	}

	for ( i = 0; i < STAR_COUNT; i++ )
	{
		Star[i].CurZ -= StepZ;
		scale = ((long)-ZMIN * 100) / ( Star[i].CurZ - ZMIN );

		NewX =(int)( CenterX + Star[i].PosX * scale / 100);
		NewY =(int)( CenterY + Star[i].PosY * scale / 100);

		if (draw) {
			glColor3f( 1.0f, 1.0f, 1.0f );
			PlotStar( Star[i].OldX, Star[i].OldY, Star[i].Type );
		}

		Star[i].OldX = NewX;
		Star[i].OldY = NewY;

		if(NewX < 639 && NewX > 1 && NewY < 479 && NewY > 1) ;
		else Star[i].CurZ = -20;

		if ( draw && Star[i].CurZ > -10 )
		{
			glColor3fv( Star[i].Color );
			PlotStar( Star[i].OldX, Star[i].OldY, Star[i].Type );
		}
	}

	glColor3f (1.0f, 1.0f, 1.0f );
}

static void NewStars( void )
{
	int i;

	for (i=0;i<STAR_COUNT;i++) if(Star[i].CurZ<-10)	InitStar(i);
}

void InitS( void )
{
	int i;

	srand((unsigned)time(NULL));

	CenterX = 320;
	CenterY = 240;
	StepZ = 5;
	g_starLastMs = 0;

	for ( i = 0; i < STAR_COUNT; i++ )
	{
		InitStar(i);
	}
}

/* Port: passo fixo de 1/60 s. Em monitores acima de 60 Hz o quadro sem passo
 * só redesenha as estrelas paradas; abaixo, os passos extras avançam sem desenhar. */
static void PlotStill( void )
{
	int i;
	for ( i = 0; i < STAR_COUNT; i++ )
		if ( Star[i].CurZ > -10 )
		{
			glColor3fv( Star[i].Color );
			PlotStar( Star[i].OldX, Star[i].OldY, Star[i].Type );
		}
	glColor3f( 1.0f, 1.0f, 1.0f );
}

void DrawStar(void)
{
	DWORD now = timeGetTime();
	int steps;
	if (g_starLastMs == 0) { g_starLastMs = now; g_starAcc = 1000.0 / 60.0; }
	g_starAcc += (double)(now - g_starLastMs);
	g_starLastMs = now;
	if (g_starAcc > 250.0) g_starAcc = 250.0;   /* depois de travadas, não dispara */
	steps = (int)(g_starAcc / (1000.0 / 60.0));
	g_starAcc -= steps * (1000.0 / 60.0);

	/* Não assumir estado GL: salva/restaura textura e cor */
	glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
	glDisable(GL_TEXTURE_2D);
	if (steps == 0) {
		PlotStill();
	} else {
		while (steps-- > 1) { MoveStars(0); NewStars(); }
		MoveStars(1);
		NewStars();
	}
	glPopAttrib();
	/* era: MoveStars(); NewStars(); (um passo por quadro desenhado) */
}
