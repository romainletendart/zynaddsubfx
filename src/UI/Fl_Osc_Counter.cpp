#include "Fl_Osc_Counter.H"

static void callback_fn(Fl_Widget *w, void *)
{
    ((Fl_Osc_Counter*)w)->cb();
}

Fl_Osc_Counter::Fl_Osc_Counter(int x, int y, int w, int h, const char *label)
    :Fl_Counter(x,y,w,h,label), Fl_Osc_Widget(this)
{
    Fl_Counter::callback(callback_fn);
}

void Fl_Osc_Counter::update(void)
{
    oscWrite(path);
}

void Fl_Osc_Counter::init(const char *path_)
{
    oscRegister(path_);
    path = path_;
}

void Fl_Osc_Counter::callback(Fl_Callback *cb, void *p)
{
    cb_data.first = cb;
    cb_data.second = p;
}

void Fl_Osc_Counter::OSC_value(char v)
{
    value(v);
}
        
void Fl_Osc_Counter::cb(void)
{
    assert(osc);

    oscWrite(path, "c", (char)(value()));
    
    if(cb_data.first)
        cb_data.first(this, cb_data.second);
}